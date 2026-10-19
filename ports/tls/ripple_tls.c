#include "ripple_tls.h"
#include "ripple_crypto.h"
#include <wolfssl/ssl.h>
#include <wolfssl/error-ssl.h>
#include <wolfssl/wolfcrypt/error-crypt.h>
#include <wolfssl/wolfio.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

struct ripple_tls {
    WOLFSSL_CTX *context;
    WOLFSSL *session;
    int fd, flags, established, aborted, io_error, io_write;
    volatile int *cancelled;
    int (*poll)(void *);
    void *poll_context;
    int profile;
    pid_t owner;
    unsigned long poll_ms, receive_ms, transmit_ms;
    unsigned long receives, transmits;
    unsigned long read_work_ms,read_wait_ms,read_calls,read_octets;
};
static char last_error[96];
static unsigned initialized;
/* One immutable anchor context per process. Sessions retain their own reference
 * so replacing the cache cannot change the policy of an existing connection. */
static WOLFSSL_CTX *trust_context;
static unsigned char *trust_contents;
static unsigned long trust_size;
static pid_t trust_owner;
static unsigned trust_cleanup_registered;
static void clear_trust(void) {
    if(trust_context)wolfSSL_CTX_free(trust_context);
    free(trust_contents);trust_context=NULL;trust_contents=NULL;trust_size=0;
}
static int same_process(RippleTLS *t) {
    if(t->owner==getpid())return 1;
    snprintf(last_error,sizeof(last_error),"TLS connection cannot be used after fork");
    errno=EIO;return 0;
}
static unsigned long stamp(void) {
    struct timespec t;
    if(clock_gettime(CLOCK_MONOTONIC,&t)<0)return 0;
    return (unsigned long)t.tv_sec*1000UL+(unsigned long)t.tv_nsec/1000000UL;
}
const char *ripple_tls_error(void) { return last_error; }
static void failure(const char *stage, int code) {
    snprintf(last_error,sizeof(last_error),"%s (TLS error %d)",stage,code);
    errno=EIO;
}
int ripple_tls_seed(unsigned char *output, unsigned long count) {
    int fd;
    ssize_t n;
    fd=open("/dev/urandom",O_RDONLY);
    if(fd<0){failure("cannot seed TLS from /dev/urandom",RNG_FAILURE_E);return RNG_FAILURE_E;}
    while(count) {
        n=read(fd,output,count>256?256:count);
        if(n<=0){close(fd);failure("cannot seed TLS from /dev/urandom",RNG_FAILURE_E);return RNG_FAILURE_E;}
        output+=n;count-=n;
    }
    if(close(fd)<0){failure("cannot seed TLS from /dev/urandom",RNG_FAILURE_E);return RNG_FAILURE_E;}
    return 0;
}
static int interrupted(RippleTLS *t) {
    unsigned long start;
    int cancel=t->aborted||(t->cancelled&&*t->cancelled);
    if(!cancel&&t->poll) {
        start=t->profile?stamp():0;
        cancel=t->poll(t->poll_context);
        if(t->profile)t->poll_ms+=stamp()-start;
    }
    if(cancel){t->aborted=1;errno=EINTR;return 1;}
    return 0;
}
static int receive(WOLFSSL *ssl,char *data,int size,void *context) {
    RippleTLS *t=context;
    ssize_t n;
    unsigned long start;
    (void)ssl;
    if(interrupted(t))return WOLFSSL_CBIO_ERR_GENERAL;
    start=t->profile?stamp():0;
    n=read(t->fd,data,size);
    if(t->profile){t->receive_ms+=stamp()-start;++t->receives;}
    if(n>0)return n;
    if(!n)return WOLFSSL_CBIO_ERR_CONN_CLOSE;
    if(errno==EAGAIN||errno==EWOULDBLOCK)return WOLFSSL_CBIO_ERR_WANT_READ;
    if(errno==EINTR)return WOLFSSL_CBIO_ERR_ISR;
    t->io_error=errno;t->io_write=0;
    return WOLFSSL_CBIO_ERR_GENERAL;
}
static int transmit(WOLFSSL *ssl,char *data,int size,void *context) {
    RippleTLS *t=context;
    ssize_t n;
    int i;
    unsigned long start;
    (void)ssl;
    if(interrupted(t))return WOLFSSL_CBIO_ERR_GENERAL;
    for(i=0;i<size;i++)if((unsigned char)data[i]>255) {
        snprintf(last_error,sizeof(last_error),"TLS non-octet wire value at %d/%d: %x",i,size,(unsigned char)data[i]);errno=EIO;return WOLFSSL_CBIO_ERR_GENERAL;
    }
    start=t->profile?stamp():0;
    n=write(t->fd,data,size);
    if(t->profile){t->transmit_ms+=stamp()-start;++t->transmits;}
    if(n>0)return n;
    if(n<0&&(errno==EAGAIN||errno==EWOULDBLOCK))return WOLFSSL_CBIO_ERR_WANT_WRITE;
    if(n<0&&errno==EINTR)return WOLFSSL_CBIO_ERR_ISR;
    if(n<0){t->io_error=errno;t->io_write=1;}
    return WOLFSSL_CBIO_ERR_GENERAL;
}
static int now(unsigned long *value) {
    struct timespec t;
    if(clock_gettime(CLOCK_MONOTONIC,&t)<0)return -1;
    *value=(unsigned long)t.tv_sec*1000UL+(unsigned long)t.tv_nsec/1000000UL;
    return 0;
}
static int wait_io(RippleTLS *t,int error,unsigned long started,unsigned long timeout) {
    unsigned long current,elapsed,remaining;
    struct timeval tv;
    fd_set read_set,write_set;
    int n;
    for(;;) {
        if(interrupted(t))return -1;
        if(now(&current)<0)return -1;
        elapsed=current-started;
        if(elapsed>=timeout){errno=ETIMEDOUT;return -1;}
        remaining=timeout-elapsed;
        if(t->poll&&remaining>50)remaining=50;
        tv.tv_sec=remaining/1000;tv.tv_usec=(remaining%1000)*1000;
        FD_ZERO(&read_set);FD_ZERO(&write_set);
        if(error==WOLFSSL_ERROR_WANT_READ)FD_SET(t->fd,&read_set);
        else if(error==WOLFSSL_ERROR_WANT_WRITE)FD_SET(t->fd,&write_set);
        else {
            if(error==SOCKET_ERROR_E&&t->io_error) {
                snprintf(last_error,sizeof(last_error),"socket %s failed: %s (TLS error %d)",
                         t->io_write?"write":"read",strerror(t->io_error),error);
                errno=t->io_error;
            } else if(!*last_error)failure("connection failed",error);
            return -1;
        }
        n=select(t->fd+1,&read_set,&write_set,NULL,&tv);
        if(n>0)return 0;
        if(!n)continue;
        if(errno!=EINTR)return -1;
    }
}
static int trust(RippleTLS *t,const char *filename) {
    struct stat st;
    unsigned char *data;
    unsigned long offset=0;
    unsigned long started=t->profile?stamp():0, loaded;
    int fd,ret;
    ssize_t n;
    pid_t owner=getpid();
    if(trust_owner!=owner){clear_trust();trust_owner=owner;}
    fd=open(filename,O_RDONLY);
    if(fd<0)return -1;
    if(fstat(fd,&st)<0||st.st_size<=0||st.st_size>512000L){close(fd);errno=EINVAL;return -1;}
    data=malloc(st.st_size);
    if(!data){close(fd);return -1;}
    while(offset<(unsigned long)st.st_size) {
        n=read(fd,data+offset,(unsigned long)st.st_size-offset>4096?4096:(unsigned long)st.st_size-offset);
        if(n<=0){free(data);close(fd);errno=EIO;return -1;}
        offset+=n;
    }
    close(fd);
    loaded=t->profile?stamp():0;
    /* Check actual contents on every connection, including same-size edits.
     * A missing/unreadable/new invalid bundle must never use stale anchors. */
    if(trust_context&&trust_size==offset&&!memcmp(trust_contents,data,offset)) {
        ret=wolfSSL_CTX_up_ref(trust_context);free(data);
        if(ret!=WOLFSSL_SUCCESS){failure("TLS context reference failed",ret);return -1;}
        t->context=trust_context;
        if(t->profile)fprintf(stderr,"TLS trust detail: %lu cells, read %lu ms, parse 0 ms (reused)\n",offset,loaded-started);
        return 0;
    }
    t->context=wolfSSL_CTX_new(wolfTLS_client_method());
    if(!t->context){free(data);failure("TLS context allocation failed",0);return -1;}
    if(wolfSSL_CTX_SetMinVersion(t->context,WOLFSSL_TLSV1_2)!=WOLFSSL_SUCCESS||
       wolfSSL_CTX_set_cipher_list(t->context,"TLS13-AES128-GCM-SHA256:ECDHE-RSA-AES128-GCM-SHA256:ECDHE-ECDSA-AES128-GCM-SHA256")!=WOLFSSL_SUCCESS) {
        free(data);failure("TLS version/cipher selection failed",0);return -1;
    }
    wolfSSL_CTX_set_verify(t->context,WOLFSSL_VERIFY_PEER,NULL);
    wolfSSL_CTX_SetIORecv(t->context,receive);wolfSSL_CTX_SetIOSend(t->context,transmit);
    /* Anchor reuse is independent of TLS session resumption. */
    wolfSSL_CTX_set_session_cache_mode(t->context,WOLFSSL_SESS_CACHE_OFF);
    ret=wolfSSL_CTX_load_verify_buffer(t->context,data,offset,WOLFSSL_FILETYPE_PEM);
    if(t->profile)fprintf(stderr,"TLS trust detail: %lu cells, read %lu ms, parse %lu ms\n",offset,loaded-started,stamp()-loaded);
    if(ret!=WOLFSSL_SUCCESS){free(data);failure("cannot load trusted certificates",ret);return -1;}
    if(!trust_cleanup_registered) {
        if(atexit(clear_trust)){free(data);errno=ENOMEM;return -1;}
        trust_cleanup_registered=1;
    }
    if(wolfSSL_CTX_up_ref(t->context)!=WOLFSSL_SUCCESS){free(data);failure("TLS context reference failed",0);return -1;}
    clear_trust();trust_context=t->context;trust_contents=data;trust_size=offset;
    return 0;
}
void ripple_tls_close(RippleTLS *t) {
    int saved=errno;
    if(!t)return;
    /* Send close_notify once on the nonblocking socket; never wait for a peer. */
    if(t->session) {
        if(t->established&&t->owner==getpid())wolfSSL_shutdown(t->session);
        wolfSSL_free(t->session);
    }
    if(t->established&&t->profile) {
        fprintf(stderr,"TLS application read: %lu octets, work %lu ms (%lu calls), wait %lu ms\n",t->read_octets,t->read_work_ms,t->read_calls,t->read_wait_ms);
        ripple_crypto_report();
    }
    if(t->context)wolfSSL_CTX_free(t->context);
    if(t->owner==getpid()&&t->fd>=0&&t->flags>=0)fcntl(t->fd,F_SETFL,t->flags);
    free(t);
    errno=saved;
}
static RippleTLS *open_client(int fd,const char *hostname,const char *ca_file,
                          unsigned long timeout,volatile int *cancelled,
                          int (*poll)(void *),void *poll_context,
                          int (*dial)(void *),void *dial_context) {
    RippleTLS *t;
    unsigned long start,profile_start,profile_end,work_start,connect_ms=0,wait_ms=0,calls=0;
    unsigned length;
    int ret,error;
    struct in_addr address;
    *last_error=0;
    if(fd<0||fd>=FD_SETSIZE||!hostname||!(length=strlen(hostname))||length>253||!timeout||timeout>600000UL){errno=EINVAL;return NULL;}
    if(!initialized) {
        if(wolfSSL_Init()!=WOLFSSL_SUCCESS){failure("TLS initialization failed",0);return NULL;}
        ripple_crypto_init();
        initialized=1;
    }
    t=calloc(1,sizeof(*t));if(!t)return NULL;
    t->fd=fd;t->flags=-1;t->cancelled=cancelled;t->poll=poll;t->poll_context=poll_context;
    t->profile=getenv("RIPPLE_TLS_PROFILE")!=NULL;t->owner=getpid();profile_start=stamp();
    now(&profile_start);
    if(trust(t,ca_file?ca_file:RIPPLE_TLS_CA_FILE)<0)goto bad;
    now(&profile_end);
    if(t->profile)fprintf(stderr,"TLS trust load: %lu ms\n",profile_end-profile_start);
    t->session=wolfSSL_new(t->context);
    if(!t->session){if(!*last_error)failure("TLS session allocation failed",0);goto bad;}
    wolfSSL_SetIOReadCtx(t->session,t);wolfSSL_SetIOWriteCtx(t->session,t);
    if(inet_pton(AF_INET,hostname,&address)==1) {
        if(wolfSSL_check_ip_address(t->session,hostname)!=WOLFSSL_SUCCESS){failure("IP verification setup failed",0);goto bad;}
    } else {
        if(wolfSSL_check_domain_name(t->session,hostname)!=WOLFSSL_SUCCESS||
           wolfSSL_UseSNI(t->session,WOLFSSL_SNI_HOST_NAME,hostname,length)!=WOLFSSL_SUCCESS){failure("hostname verification setup failed",0);goto bad;}
    }
    if(dial&&dial(dial_context)){failure("TCP connection failed",0);goto bad;}
    t->flags=fcntl(fd,F_GETFL,0);
    if(t->flags<0||fcntl(fd,F_SETFL,t->flags|O_NONBLOCK)<0||now(&start)<0)goto bad;
    for(;;) {
        if(interrupted(t))goto bad;
        work_start=t->profile?stamp():0;
        ret=wolfSSL_connect(t->session);
        if(t->profile){connect_ms+=stamp()-work_start;++calls;}
        if(ret==WOLFSSL_SUCCESS){
            t->established=1;
            if(t->profile){
                now(&profile_end);fprintf(stderr,"TLS handshake: %lu ms\n",profile_end-start);
                fprintf(stderr,"TLS handshake detail: connect %lu ms (%lu calls), wait %lu ms, receive %lu ms (%lu calls), send %lu ms (%lu calls), keyboard %lu ms\n",connect_ms,calls,wait_ms,t->receive_ms,t->receives,t->transmit_ms,t->transmits,t->poll_ms);
                ripple_crypto_report();
            }
            return t;
        }
        error=wolfSSL_get_error(t->session,ret);
        work_start=t->profile?stamp():0;
        ret=wait_io(t,error,start,timeout);
        if(t->profile)wait_ms+=stamp()-work_start;
        if(ret<0)goto bad;
    }
bad:
    if(t->profile) {
        int saved=errno;
        fprintf(stderr,"TLS failed after %lu ms: connect %lu ms (%lu calls), wait %lu ms, receive %lu ms (%lu calls), send %lu ms (%lu calls)\n",
                stamp()-profile_start,connect_ms,calls,wait_ms,t->receive_ms,t->receives,t->transmit_ms,t->transmits);
        ripple_crypto_report();errno=saved;
    }
    ripple_tls_close(t);return NULL;
}
RippleTLS *ripple_tls_open(int fd,const char *hostname,const char *ca_file,unsigned long timeout,volatile int *cancelled) {
    return open_client(fd,hostname,ca_file,timeout,cancelled,NULL,NULL,NULL,NULL);
}
RippleTLS *ripple_tls_open_poll(int fd,const char *hostname,const char *ca_file,unsigned long timeout,int (*poll)(void *),void *context) {
    return open_client(fd,hostname,ca_file,timeout,NULL,poll,context,NULL,NULL);
}
RippleTLS *ripple_tls_open_dial(int fd,const char *hostname,const char *ca_file,unsigned long timeout,int (*dial)(void *),void *context) {
    return open_client(fd,hostname,ca_file,timeout,NULL,NULL,NULL,dial,context);
}
ssize_t ripple_tls_read(RippleTLS *t,void *data,size_t count,unsigned long timeout) {
    unsigned long start,work;
    int ret,error;
    if(!same_process(t))return -1;
    if(!count)return 0;
    if(now(&start)<0)return -1;
    for(;;) {
        work=t->profile?stamp():0;
        ret=wolfSSL_read(t->session,data,count>16384?16384:count);
        if(t->profile){t->read_work_ms+=stamp()-work;++t->read_calls;}
        if(ret>0){if(t->profile)t->read_octets+=ret;return ret;}
        error=wolfSSL_get_error(t->session,ret);
        if(error==WOLFSSL_ERROR_ZERO_RETURN)return 0;
        work=t->profile?stamp():0;
        ret=wait_io(t,error,start,timeout);
        if(t->profile)t->read_wait_ms+=stamp()-work;
        if(ret<0)return -1;
    }
}
int ripple_tls_write(RippleTLS *t,const void *data,size_t count,unsigned long timeout) {
    unsigned long start;
    int ret,error;
    if(!same_process(t))return -1;
    if(now(&start)<0)return -1;
    while(count) {
        ret=wolfSSL_write(t->session,data,count>16384?16384:count);
        if(ret>0){data=(const char *)data+ret;count-=ret;continue;}
        error=wolfSSL_get_error(t->session,ret);
        if(wait_io(t,error,start,timeout)<0)return -1;
    }
    return 0;
}
unsigned ripple_tls_pending(RippleTLS *t) {return same_process(t)?wolfSSL_pending(t->session):0;}
