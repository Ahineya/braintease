/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Foreground, key-only SSH service. Service supervision belongs to the OS. */
#include <wolfssh/ssh.h>
#include <wolfssh/wolfsftp.h>
#include <wolfssl/wolfcrypt/asn_public.h>
#include <wolfssl/wolfcrypt/coding.h>
#include <wolfssl/wolfcrypt/ecc.h>
#include <wolfssl/wolfcrypt/random.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <unistd.h>
#include <time.h>
#include <termios.h>
#include <sys/stat.h>
#include <sys/select.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <dirent.h>
#include "file-limits.h"
#include "pty-limits.h"
#include "ripple_crypto.h"
#define RH_OPEN(p) open(p,O_RDONLY)
#define RH_READ(fd,p,n) read(fd,p,n)
#define RH_CLOSE(fd) close(fd)
#define RH_CHDIR(p) chdir(p)
#include "root-home.h"

#define CLIENTS 4
#define BUFFER 4096
#define KEYS 16
#define KEY_SIZE 1024
#define COMMAND_SIZE 2048
static const char algorithm[]="ecdsa-sha2-nistp256";
static const char accepted_algorithms[]="ecdsa-sha2-nistp256,ecdsa-sha2-nistp384,ecdsa-sha2-nistp521,ssh-ed25519,rsa-sha2-512,rsa-sha2-256";
static int accepted_algorithm(const byte *name,unsigned length) {
    static const char *names[]={"ecdsa-sha2-nistp256","ecdsa-sha2-nistp384","ecdsa-sha2-nistp521","ssh-ed25519","ssh-rsa","rsa-sha2-512","rsa-sha2-256"};
    unsigned i;for(i=0;i<7;i++)if(length==strlen(names[i])&&!memcmp(name,names[i],length))return 1;return 0;
}
static volatile int stopping;
static int children[CLIENTS];
static const char *hostkey="/etc/ssh/hostkey.der";
static const char *authorized="/root/.ssh/authorized_keys";
static void wipe(void *data,unsigned size) {
    volatile byte *p=data;
    while(size--)*p++=0;
}
struct allowed_key { unsigned length; byte blob[KEY_SIZE]; };
struct block { byte data[BUFFER]; unsigned at,length; };
struct session {
    WOLFSSH *ssh;
    WOLFSSH_CHANNEL *channel;
    int fd,child,closed,started,pty,master,input,output,errors;
    int out_eof,err_eof,in_eof,reaped,status,exit_sent,sftp,sftp_ready;
    unsigned key_count,columns,rows,mode_count;
    byte modes[1024];
    struct allowed_key keys[KEYS];
    struct block incoming,outgoing,extended;
};

static unsigned long now_ms(void) {
    struct timespec t;
    if(clock_gettime(CLOCK_MONOTONIC,&t)<0)return 0;
    return (unsigned long)t.tv_sec*1000UL+(unsigned long)t.tv_nsec/1000000UL;
}
static void caught(int sig) {stopping=sig;signal(sig,caught);}
static void changed(int sig) {signal(sig,changed);}
static int notify_ready(int listener) {
    const char *text=getenv("SVC_READY_FD");char *end;unsigned long fd;
    if(!text)return 0;
    fd=strtoul(text,&end,10);
    if(!*text||*end||fd<3||fd>=RIPPLE_FILE_LIMIT||(int)fd==listener){errno=EINVAL;return -1;}
    if(write((int)fd,"R",1)!=1){close((int)fd);return -1;}
    return close((int)fd);
}
static int again(void) {return errno==EINTR||errno==EAGAIN;}
static int transient(int r) {
    return r==WS_SUCCESS||r==WS_WANT_READ||r==WS_WANT_WRITE||r==WS_REKEYING||r==WS_WINDOW_FULL;
}
static int waiting(int r) {return r==WS_WANT_READ||r==WS_WANT_WRITE||r==WS_REKEYING||r==WS_WINDOW_FULL;}
static int wait_fd(int fd,int writing,unsigned long timeout) {
    fd_set r,w;struct timeval tv;
    FD_ZERO(&r);FD_ZERO(&w);
    if(writing)FD_SET(fd,&w);else FD_SET(fd,&r);
    tv.tv_sec=timeout/1000;tv.tv_usec=(timeout%1000)*1000;
    return select(fd+1,&r,&w,NULL,&tv);
}
static int io_read(WOLFSSH *ssh,void *data,word32 size,void *context) {
    struct session *s=context;ssize_t n;
    (void)ssh;
    n=read(s->fd,data,size>16384UL?16384:(unsigned)size);
    if(n>0)return n;
    if(!n)return WS_CBIO_ERR_CONN_CLOSE;
    if(errno==EINTR)return WS_CBIO_ERR_ISR;
    return errno==EAGAIN?WS_CBIO_ERR_WANT_READ:WS_CBIO_ERR_GENERAL;
}
static int io_write(WOLFSSH *ssh,void *data,word32 size,void *context) {
    struct session *s=context;byte *p=data;unsigned i,n=size>16384UL?16384:(unsigned)size;
    ssize_t result;
    (void)ssh;
    for(i=0;i<n;i++)if(p[i]>255)return WS_CBIO_ERR_GENERAL;
    result=write(s->fd,p,n);
    if(result>0)return result;
    if(result<0&&errno==EINTR)return WS_CBIO_ERR_ISR;
    return result<0&&errno==EAGAIN?WS_CBIO_ERR_WANT_WRITE:WS_CBIO_ERR_GENERAL;
}
/* All credential path components must be root-owned and not writable by
 * another user. Refuse symlinks, and verify the descriptor after opening. */
static int credential_path(const char *path) {
    char copy[512];struct stat st;unsigned i,component=1,n=strlen(path);
    if(!n||n>=sizeof(copy)||path[0]!='/'){errno=EINVAL;return -1;}
    memcpy(copy,path,n+1);
    for(i=1;i<=n;i++)if(copy[i]=='/'||!copy[i]) {
        char saved=copy[i];if(i-component>NAME_MAX){errno=ENAMETOOLONG;return -1;}component=i+1;copy[i]=0;
        if(lstat(copy,&st)<0)return -1;
        copy[i]=saved;
        if(st.st_uid||st.st_mode&022||S_ISLNK(st.st_mode)||
           (i<n?!S_ISDIR(st.st_mode):!S_ISREG(st.st_mode))){errno=EACCES;return -1;}
    }
    return 0;
}
static byte *read_credential(const char *path,word32 *length,int private) {
    struct stat before,after;byte *p=NULL;unsigned at=0,i;int fd=-1;ssize_t n;
    if(credential_path(path)||lstat(path,&before)<0)return NULL;
    if(private&&(before.st_mode&077)){errno=EACCES;return NULL;}
    if(before.st_size<=0||before.st_size>16384L){errno=EFBIG;return NULL;}
    fd=open(path,O_RDONLY);if(fd<0)return NULL;
    if(fstat(fd,&after)<0||after.st_ino!=before.st_ino||after.st_dev!=before.st_dev||
       after.st_size!=before.st_size||after.st_mode!=before.st_mode||after.st_uid!=before.st_uid)goto bad;
    p=malloc((unsigned)after.st_size+1);if(!p)goto bad;
    while(at<(unsigned)after.st_size) {
        n=read(fd,p+at,(unsigned)after.st_size-at);
        if(n<0&&errno==EINTR)continue;
        if(n<=0)goto bad;
        for(i=0;i<(unsigned)n;i++)if(p[at+i]>255){errno=EINVAL;goto bad;}
        at+=n;
    }
    if(close(fd)<0){fd=-1;goto bad;}fd=-1;p[at]=0;*length=at;return p;
bad:
    if(fd>=0)close(fd);
    if(p){wipe(p,(unsigned)after.st_size);free(p);}
    return NULL;
}
static int read_keys(struct session *s) {
    byte *data;word32 length,out;
    unsigned at=0,begin,end,n,type_begin,type_length;
    data=read_credential(authorized,&length,0);if(!data)return -1;
    while(at<length) {
        begin=at;while(at<length&&data[at]!='\n')at++;end=at;if(at<length)at++;
        while(begin<end&&(data[begin]==' '||data[begin]=='\t'||data[begin]=='\r'))begin++;
        if(begin==end||data[begin]=='#')continue;
        n=begin;while(n<end&&data[n]!=' '&&data[n]!='\t')n++;
        type_begin=begin;type_length=n-begin;
        if(!accepted_algorithm(data+begin,type_length)||
           (type_length>=9&&!memcmp(data+begin,"rsa-sha2-",9))) {
            /* Unsupported key types are inert; options are never discarded. */
            if((n-begin>=4&&!memcmp(data+begin,"ssh-",4))||
               (n-begin>=11&&!memcmp(data+begin,"ecdsa-sha2-",11)))continue;
            errno=EINVAL;goto bad;
        }
        begin=n;while(begin<end&&(data[begin]==' '||data[begin]=='\t'))begin++;
        n=begin;while(n<end&&data[n]!=' '&&data[n]!='\t'&&data[n]!='\r')n++;
        if(s->key_count==KEYS||n==begin||n-begin>1536){errno=EINVAL;goto bad;}
        out=KEY_SIZE;
        if(Base64_Decode(data+begin,n-begin,s->keys[s->key_count].blob,&out)){errno=EINVAL;goto bad;}
        {byte *blob=s->keys[s->key_count].blob;
         word32 token=out<4?0:((word32)blob[0]<<24)|((word32)blob[1]<<16)|((word32)blob[2]<<8)|blob[3];
         if(token!=type_length||out<4+type_length||memcmp(blob+4,data+type_begin,type_length)){errno=EINVAL;goto bad;}}
        s->keys[s->key_count++].length=out;
    }
    free(data);if(s->key_count)return 0;errno=EACCES;return -1;
bad:free(data);return -1;
}
static int auth_types(WOLFSSH *ssh,void *context) {(void)ssh;(void)context;return WOLFSSH_USERAUTH_PUBLICKEY;}
static int authenticate(byte type,WS_UserAuthData *auth,void *context) {
    struct session *s=context;unsigned i;
    if(type!=WOLFSSH_USERAUTH_PUBLICKEY)return WOLFSSH_USERAUTH_INVALID_AUTHTYPE;
    if(auth->usernameSz!=4||memcmp(auth->username,"root",4))return WOLFSSH_USERAUTH_INVALID_USER;
    if((auth->sf.publicKey.publicKeyTypeSz==7&&!memcmp(auth->sf.publicKey.publicKeyType,"ssh-rsa",7))||auth->sf.publicKey.isCert||!accepted_algorithm(auth->sf.publicKey.publicKeyType,auth->sf.publicKey.publicKeyTypeSz))return WOLFSSH_USERAUTH_INVALID_PUBLICKEY;
    for(i=0;i<s->key_count;i++)if(s->keys[i].length==auth->sf.publicKey.publicKeySz&&
       !memcmp(s->keys[i].blob,auth->sf.publicKey.publicKey,s->keys[i].length))return WOLFSSH_USERAUTH_SUCCESS;
    return WOLFSSH_USERAUTH_INVALID_PUBLICKEY;
}
static word32 big32(const byte *p) {
    return ((word32)p[0]<<24)|((word32)p[1]<<16)|((word32)p[2]<<8)|p[3];
}
static int resize(struct session *s) {
    struct winsize size;
    if(s->master<0)return 0;
    memset(&size,0,sizeof(size));size.ws_col=s->columns;size.ws_row=s->rows;
    return ioctl(s->master,TIOCSWINSZ,&size);
}
static void modes(struct session *s,int fd) {
    struct termios t;unsigned at,op;word32 value;
    if(tcgetattr(fd,&t)<0)return;
    for(at=0;at<s->mode_count;) {
        op=s->modes[at++];if(!op||op>=160)break;
        if(s->mode_count-at<4)break;
        value=big32(s->modes+at);at+=4;
#define CC(code,index) case code:t.c_cc[index]=value&255;break
#define FLAG(code,field,bit) case code:if(value)t.field|=bit;else t.field&=~bit;break
        switch(op) {
        CC(1,VINTR);CC(2,VQUIT);CC(3,VERASE);CC(4,VKILL);CC(5,VEOF);CC(6,VEOL);CC(10,VSUSP);CC(14,VDISCARD);CC(15,VLNEXT);
        FLAG(30,c_iflag,IGNPAR);FLAG(31,c_iflag,PARMRK);FLAG(32,c_iflag,INPCK);FLAG(33,c_iflag,ISTRIP);
        FLAG(34,c_iflag,INLCR);FLAG(35,c_iflag,IGNCR);FLAG(36,c_iflag,ICRNL);FLAG(38,c_iflag,IXON);FLAG(40,c_iflag,IXOFF);
        FLAG(50,c_lflag,ISIG);FLAG(51,c_lflag,ICANON);FLAG(53,c_lflag,ECHO);FLAG(54,c_lflag,ECHOE);
        FLAG(55,c_lflag,ECHOK);FLAG(56,c_lflag,ECHONL);FLAG(57,c_lflag,NOFLSH);FLAG(58,c_lflag,TOSTOP);FLAG(59,c_lflag,IEXTEN);
        FLAG(70,c_oflag,OPOST);FLAG(72,c_oflag,ONLCR);FLAG(73,c_oflag,OCRNL);FLAG(75,c_oflag,ONLRET);
        default:break;
        }
#undef CC
#undef FLAG
    }
    tcsetattr(fd,TCSANOW,&t);
}
static int request_any(WOLFSSH_CHANNEL *channel,const byte *type,word32 type_n,
                       const byte *data,word32 size,int reply,void *context) {
    struct session *s=context;word32 n,at,cols,rows;
    (void)channel;(void)reply;
    if(type_n==7&&!memcmp(type,"pty-req",7)) {
        if(s->started||s->pty||size<24)return WOLFSSH_REQ_REJECT;
        n=big32(data);if(n>64||n>size-24)return WOLFSSH_REQ_REJECT;
        at=4+n;cols=big32(data+at);rows=big32(data+at+4);
        if(!cols)cols=80;if(!rows)rows=24;
        if(cols>1000||rows>1000)return WOLFSSH_REQ_REJECT;
        at+=16;n=big32(data+at);at+=4;
        if(n>sizeof(s->modes)||n!=size-at)return WOLFSSH_REQ_REJECT;
        s->columns=cols;s->rows=rows;s->mode_count=n;memcpy(s->modes,data+at,n);s->pty=1;
        return WOLFSSH_REQ_UNHANDLED;
    }
    if(type_n==13&&!memcmp(type,"window-change",13)) {
        if(!s->pty||size!=16)return WOLFSSH_REQ_REJECT;
        cols=big32(data);rows=big32(data+4);
        if(!cols||!rows||cols>1000||rows>1000)return WOLFSSH_REQ_REJECT;
        s->columns=cols;s->rows=rows;
        return resize(s)?WOLFSSH_REQ_REJECT:WOLFSSH_REQ_UNHANDLED;
    }
    if((type_n==5&&!memcmp(type,"shell",5))||(type_n==4&&!memcmp(type,"exec",4))||
       (type_n==9&&!memcmp(type,"subsystem",9)))return WOLFSSH_REQ_UNHANDLED;
    return WOLFSSH_REQ_REJECT;
}
static void close_fds(int keep) {unsigned i;for(i=3;i<RIPPLE_FILE_LIMIT;i++)if((int)i!=keep)close(i);}
static int open_channel(WOLFSSH_CHANNEL *channel,void *context) {
    struct session *s=context;
    if(s->channel||strcmp(wolfSSH_ChannelGetType(channel),"session"))return WS_INVALID_CHANTYPE;
    s->channel=channel;return WS_SUCCESS;
}
static int start_program(WOLFSSH_CHANNEL *channel,void *context) {
    struct session *s=context;
    const char *cmd=wolfSSH_ChannelGetSessionCommand(channel);
    word32 cmd_size=wolfSSH_ChannelGetSessionCommandSz(channel);
    int incoming[2]={-1,-1},outgoing[2]={-1,-1},errors[2]={-1,-1},pid,slave=-1;
    unsigned i;char masterpath[]="/dev/ptyp0",slavepath[]="/dev/ttyp0";
    WS_SessionType type=wolfSSH_ChannelGetSessionType(channel);
    if(s->started||cmd_size>=COMMAND_SIZE||
       (type==WOLFSSH_SESSION_EXEC&&(!cmd||strlen(cmd)!=cmd_size)))return WS_BAD_ARGUMENT;
    if(type==WOLFSSH_SESSION_SUBSYSTEM) {
        if(s->pty||!cmd||cmd_size!=4||memcmp(cmd,"sftp",4))return WS_BAD_ARGUMENT;
        if(root_home_start()<0||wolfSSH_SFTP_SetDefaultPath(s->ssh,root_home))return WS_BAD_FILE_E;
        s->started=s->sftp=1;return WS_SUCCESS;
    }
    if(s->pty) {
        for(i=0;i<RIPPLE_PTY_COUNT;i++) {
            masterpath[9]=slavepath[9]=RIPPLE_PTY_DIGIT(i);
            s->master=open(masterpath,O_RDWR|O_NOCTTY|O_NONBLOCK);
            if(s->master>=0)break;
            if(errno!=EBUSY)return WS_RESOURCE_E;
        }
        if(s->master<0||resize(s)<0)goto bad;
    } else {
        if(pipe(incoming)||pipe(outgoing)||pipe(errors))goto bad;
        if(fcntl(incoming[1],F_SETFL,O_NONBLOCK)<0||fcntl(outgoing[0],F_SETFL,O_NONBLOCK)<0||
           fcntl(errors[0],F_SETFL,O_NONBLOCK)<0)goto bad;
    }
    pid=fork();if(pid<0)goto bad;
    if(!pid) {
        char *shell_args[]={"sh",NULL,NULL,NULL};
        char *env[]={"PATH=/bin:/usr/bin",root_home_environment,"USER=root","LOGNAME=root",
                     "TERM=xterm","SHELL=/bin/sh","EDITOR=/bin/vi",NULL};
        signal(SIGINT,SIG_DFL);signal(SIGTERM,SIG_DFL);signal(SIGHUP,SIG_DFL);
        signal(SIGCHLD,SIG_DFL);signal(SIGPIPE,SIG_DFL);
        if(setsid()<0)_exit(125);
        if(s->pty) {
            close(s->master);slave=open(slavepath,O_RDWR);if(slave<0)_exit(125);
            modes(s,slave);
            if(dup2(slave,0)<0||dup2(slave,1)<0||dup2(slave,2)<0)_exit(125);
        } else if(dup2(incoming[0],0)<0||dup2(outgoing[1],1)<0||dup2(errors[1],2)<0)_exit(125);
        close_fds(-1);
        if(root_home_start()<0)_exit(125);
        if(type==WOLFSSH_SESSION_EXEC){shell_args[1]="-c";shell_args[2]=(char *)cmd;}
        execve("/bin/sh",shell_args,env);_exit(127);
    }
    s->child=pid;s->started=1;
    if(s->pty){s->input=s->output=s->master;s->errors=-1;s->err_eof=1;}
    else {
        close(incoming[0]);close(outgoing[1]);close(errors[1]);
        s->input=incoming[1];s->output=outgoing[0];s->errors=errors[0];
    }
    return WS_SUCCESS;
bad:
    for(i=0;i<2;i++){if(incoming[i]>=0)close(incoming[i]);if(outgoing[i]>=0)close(outgoing[i]);if(errors[i]>=0)close(errors[i]);}
    if(s->master>=0){close(s->master);s->master=-1;}
    return WS_RESOURCE_E;
}
static int fill_output(int fd,struct block *b,int *eof) {
    ssize_t n;
    if(*eof||b->length)return 0;
    n=read(fd,b->data,sizeof(b->data));
    if(n>0){b->length=n;b->at=0;return 1;}
    if(!n||(n<0&&errno==EIO)){*eof=1;return 1;}
    return again()?0:-1;
}
static int send_output(struct session *s,struct block *b,int extended) {
    int n;
    if(!b->length)return 0;
    n=extended?wolfSSH_extended_data_send(s->ssh,b->data+b->at,b->length):
               wolfSSH_stream_send(s->ssh,b->data+b->at,b->length);
    if(n>0){b->at+=n;b->length-=n;return 1;}
    if(n==WS_FATAL_ERROR)n=wolfSSH_get_error(s->ssh);
    return transient(n)?0:-1;
}
static int run_session(struct session *s) {
    unsigned long started=now_ms(),ending=0;
    int r,error,n,progress;
    word32 channel;
    for(;;) {
        if(stopping||now_ms()-started>=30000UL){errno=ETIMEDOUT;return -1;}
        r=wolfSSH_accept(s->ssh);if(r==WS_SUCCESS)break;
        error=wolfSSH_get_error(s->ssh);
        if(!transient(error)){fprintf(stderr,"sshd: handshake: %s (%d)\n",wolfSSH_ErrorToName(error),error);return -1;}
        wait_fd(s->fd,error==WS_WANT_WRITE,100);
    }
    while(!stopping&&!s->closed) {
        progress=0;
        r=wolfSSH_worker(s->ssh,&channel);
        if(r==WS_CHANNEL_CLOSED||r==WS_DISCONNECT)break;
        error=wolfSSH_get_error(s->ssh);
        if(r==WS_FATAL_ERROR&&!transient(error)){fprintf(stderr,"sshd: connection: %s (%d)\n",wolfSSH_ErrorToName(error),error);break;}
        if(!transient(r)&&r!=WS_CHAN_RXD&&r!=WS_EXTDATA&&r!=WS_EOF&&r!=WS_FATAL_ERROR){fprintf(stderr,"sshd: transport event %d\n",r);break;}
        if(s->sftp) {
            r=s->sftp_ready?wolfSSH_SFTP_read(s->ssh):wolfSSH_SFTP_accept(s->ssh);
            if(r==WS_SFTP_COMPLETE){s->sftp_ready=1;progress=1;}
            else if(r==WS_EOF||(r==WS_FATAL_ERROR&&wolfSSH_get_error(s->ssh)==WS_EOF)) {
                wolfSSH_stream_exit(s->ssh,0);
                started=now_ms();
                while(wolfSSH_OutputPending(s->ssh)&&now_ms()-started<5000UL&&!stopping) {
                    wolfSSH_worker(s->ssh,NULL);wait_fd(s->fd,1,100);
                }
                break;
            } else if(r==WS_SUCCESS)progress=1;
            else {
                error=wolfSSH_get_error(s->ssh);
                if(!waiting(r)&&!(r==WS_FATAL_ERROR&&waiting(error))){fprintf(stderr,"sshd: SFTP: %s (%d, %d)\n",wolfSSH_ErrorToName(error?error:r),r,error);break;}
            }
        } else if(!s->started) {
            if(now_ms()-started>=30000UL)break;
        } else {
            if(!s->incoming.length&&!s->in_eof) {
                n=wolfSSH_ChannelRead(s->channel,s->incoming.data,sizeof(s->incoming.data));
                if(n>0){s->incoming.length=n;s->incoming.at=0;progress=1;}
                else if(n<0&&!transient(n))break;
                if(!n&&wolfSSH_ChannelGetEof(s->channel))s->in_eof=1;
            }
            if(s->incoming.length) {
                if(s->input<0){s->incoming.length=0;progress=1;}
                else {
                    n=write(s->input,s->incoming.data+s->incoming.at,s->incoming.length);
                    if(n>0){s->incoming.at+=n;s->incoming.length-=n;progress=1;}
                    else if(n<0&&errno==EPIPE&&!s->pty) {
                        /* A command may close stdin while still producing
                         * output. Drain that output and preserve its status. */
                        close(s->input);s->input=-1;s->incoming.length=0;progress=1;
                    } else if(n<0&&!again())break;
                }
            } else if(s->in_eof&&s->input>=0&&!s->pty){close(s->input);s->input=-1;}
            n=fill_output(s->output,&s->outgoing,&s->out_eof);if(n<0)break;progress|=n;
            n=fill_output(s->errors,&s->extended,&s->err_eof);if(n<0)break;progress|=n;
            n=send_output(s,&s->outgoing,0);if(n<0)break;progress|=n;
            n=send_output(s,&s->extended,1);if(n<0)break;progress|=n;
            if(!s->reaped&&waitpid(s->child,&s->status,WNOHANG)==s->child){s->reaped=1;progress=1;}
            if(s->reaped&&s->out_eof&&s->err_eof&&!s->outgoing.length&&!s->extended.length&&
               !wolfSSH_RekeyPending(s->ssh)) {
                if(!ending)ending=now_ms();
                if(!s->exit_sent) {
                    n=WIFEXITED(s->status)?WEXITSTATUS(s->status):128+WTERMSIG(s->status);
                    r=wolfSSH_stream_exit(s->ssh,n);
                    if(r==WS_SUCCESS)s->exit_sent=2;
                    else if(r==WS_WANT_WRITE)s->exit_sent=1;
                    else if(!transient(r))break;
                } else if(s->exit_sent==1&&!wolfSSH_OutputPending(s->ssh)) {
                    r=wolfSSH_ChannelExit(s->channel);
                    if(r==WS_SUCCESS)s->exit_sent=2;
                    else if(!transient(r))break;
                }
                if(now_ms()-ending>=5000UL)break;
            }
        }
        if(!progress) {
            fd_set reads,writes;struct timeval tv;int maximum=s->fd;
            FD_ZERO(&reads);FD_ZERO(&writes);FD_SET(s->fd,&reads);
            if(wolfSSH_OutputPending(s->ssh))FD_SET(s->fd,&writes);
            if(s->started&&!s->sftp) {
                if(!s->out_eof&&!s->outgoing.length){FD_SET(s->output,&reads);if(s->output>maximum)maximum=s->output;}
                if(!s->err_eof&&!s->extended.length){FD_SET(s->errors,&reads);if(s->errors>maximum)maximum=s->errors;}
                if(s->incoming.length&&s->input>=0){FD_SET(s->input,&writes);if(s->input>maximum)maximum=s->input;}
            }
            tv.tv_sec=0;tv.tv_usec=100000;
            if(select(maximum+1,&reads,&writes,NULL,&tv)<0&&errno!=EINTR)break;
        }
    }
    return 0;
}
static void cleanup(struct session *s) {
    unsigned long until;
    if(s->master>=0)close(s->master);
    else {if(s->input>=0)close(s->input);if(s->output>=0)close(s->output);if(s->errors>=0)close(s->errors);}
    if(s->child) {
        kill(-s->child,SIGHUP);kill(-s->child,SIGCONT);
        if(!s->reaped) {
            until=now_ms()+1000;
            while(waitpid(s->child,NULL,WNOHANG)!=s->child&&now_ms()<until)usleep(10000);
            kill(-s->child,SIGKILL);while(waitpid(s->child,NULL,0)<0&&errno==EINTR);
        }
    }
    if(s->ssh)wolfSSH_free(s->ssh);
    memset(s,0,sizeof(*s));free(s);
}
static int keygen(const char *path) {
    WC_RNG rng;ecc_key key;byte der[256];int r,fd=-1,n,i,initialized;
    if(wc_InitRng(&rng)){fputs("sshd: entropy unavailable for key generation\n",stderr);return 1;}
    r=wc_ecc_init(&key);
    initialized=!r;
    if(!r)r=wc_ecc_make_key(&rng,32,&key);
    if(!r){r=wc_EccKeyToDer(&key,der,sizeof(der));if(r>0){n=r;r=0;}else n=0;}
    else n=0;
    if(!r){
        fd=open(path,O_WRONLY|O_CREAT|O_EXCL|O_SYNC,0600);
        if(fd<0)r=-1;
        else {for(i=0;i<n;){int w=write(fd,der+i,n-i);if(w<0&&errno==EINTR)continue;if(w<=0){r=-1;break;}i+=w;}
              if(close(fd)<0)r=-1;}
        if(r&&fd>=0)unlink(path);
    }
    if(initialized)wc_ecc_free(&key);wc_FreeRng(&rng);wipe(der,sizeof(der));
    if(r)fprintf(stderr,"sshd: key generation failed (%d): %s\n",r,strerror(errno));
    return r?1:0;
}
int main(int argc,char **argv) {
    unsigned port=22,i;char *end;unsigned long number;word32 key_size;
    byte *key;WOLFSSH_CTX *ctx;struct sockaddr_in address;int listener,fd,pid,status;
    if(argc==3&&!strcmp(argv[1],"--keygen")) {
        if(wolfSSH_Init()!=WS_SUCCESS)return 1;
        ripple_crypto_init();return keygen(argv[2]);
    }
    for(i=1;i<(unsigned)argc;i++) {
        if(!strcmp(argv[i],"--help"))goto usage;
        if(i+1>=(unsigned)argc)goto usage;
        if(!strcmp(argv[i],"-p")) {
            number=strtoul(argv[++i],&end,10);
            if(!*argv[i]||*end||!number||number>65535UL)goto usage;port=number;
        } else if(!strcmp(argv[i],"--host-key"))hostkey=argv[++i];
        else if(!strcmp(argv[i],"--authorized-keys"))authorized=argv[++i];
        else goto usage;
    }
    if(geteuid()!=0){fputs("sshd: this profile serves the root account and must run as root\n",stderr);return 1;}
    key=read_credential(hostkey,&key_size,1);if(!key){perror("sshd: host key");return 1;}
    if(wolfSSH_Init()!=WS_SUCCESS){free(key);return 1;}
    ctx=wolfSSH_CTX_new(WOLFSSH_ENDPOINT_SERVER,NULL);if(!ctx){free(key);return 1;}
    status=wolfSSH_CTX_UsePrivateKey_buffer(ctx,key,key_size,WOLFSSH_FORMAT_ASN1);
    wipe(key,key_size);free(key);
    if(status||wolfSSH_CTX_SetAlgoListKex(ctx,"ecdh-sha2-nistp256")||
       wolfSSH_CTX_SetAlgoListKey(ctx,algorithm)||wolfSSH_CTX_SetAlgoListKeyAccepted(ctx,accepted_algorithms)||
       wolfSSH_CTX_SetAlgoListCipher(ctx,"aes128-gcm@openssh.com")) {
        fprintf(stderr,"sshd: key/profile initialization failed (%d)\n",status);wolfSSH_CTX_free(ctx);return 1;
    }
    wolfSSH_CTX_SetAppChannels(ctx,1);wolfSSH_SetUserAuth(ctx,authenticate);wolfSSH_SetUserAuthTypes(ctx,auth_types);
    wolfSSH_SetIORecv(ctx,io_read);wolfSSH_SetIOSend(ctx,io_write);
    wolfSSH_CTX_SetChannelOpenCb(ctx,open_channel);wolfSSH_CTX_SetChannelReqAnyCb(ctx,request_any);
    wolfSSH_CTX_SetChannelReqShellCb(ctx,start_program);wolfSSH_CTX_SetChannelReqExecCb(ctx,start_program);
    wolfSSH_CTX_SetChannelReqSubsysCb(ctx,start_program);
    {
        struct session *check=calloc(1,sizeof(*check));
        if(!check||read_keys(check)<0){perror("sshd: authorized keys");free(check);wolfSSH_CTX_free(ctx);return 1;}
        free(check);
    }
    listener=socket(AF_INET,SOCK_STREAM,0);if(listener<0){perror("sshd: socket");wolfSSH_CTX_free(ctx);return 1;}
    memset(&address,0,sizeof(address));address.sin_family=AF_INET;address.sin_port=htons(port);
    if(bind(listener,(void *)&address,sizeof(address))||listen(listener,CLIENTS)){perror("sshd: listen");close(listener);wolfSSH_CTX_free(ctx);return 1;}
    signal(SIGCHLD,changed);signal(SIGINT,caught);signal(SIGTERM,caught);signal(SIGHUP,caught);signal(SIGPIPE,SIG_IGN);
    if(notify_ready(listener)<0){perror("sshd: readiness notification");close(listener);wolfSSH_CTX_free(ctx);return 1;}
    printf("sshd: listening on port %u (public-key authentication)\n",port);fflush(stdout);
    while(!stopping) {
        for(i=0;i<CLIENTS;i++)if(children[i]&&waitpid(children[i],NULL,WNOHANG)==children[i])children[i]=0;
        fd=accept(listener,NULL,NULL);if(fd<0){if(errno==EINTR)continue;perror("sshd: accept");break;}
        for(i=0;i<CLIENTS;i++)if(!children[i])break;
        if(i==CLIENTS){close(fd);continue;}
        pid=fork();
        if(!pid) {
            struct session *s=calloc(1,sizeof(*s));close(listener);stopping=0;
            if(!s){close(fd);_exit(1);}
            s->fd=fd;s->master=s->input=s->output=s->errors=-1;s->columns=80;s->rows=24;
            close_fds(fd);ripple_crypto_init();
            if(read_keys(s)<0){perror("sshd: authorized keys");cleanup(s);close(fd);_exit(1);}
            s->ssh=wolfSSH_new(ctx);if(!s->ssh){cleanup(s);close(fd);_exit(1);}
            wolfSSH_set_fd(s->ssh,fd);wolfSSH_SetIOReadCtx(s->ssh,s);wolfSSH_SetIOWriteCtx(s->ssh,s);
            wolfSSH_SetUserAuthCtx(s->ssh,s);wolfSSH_SetChannelOpenCtx(s->ssh,s);wolfSSH_SetChannelReqCtx(s->ssh,s);
            if(fcntl(fd,F_SETFL,O_NONBLOCK)<0){perror("sshd: nonblocking socket");status=-1;}
            else status=run_session(s);
            cleanup(s);close(fd);wolfSSH_CTX_free(ctx);_exit(status?1:0);
        }
        if(pid>0)children[i]=pid;else perror("sshd: fork");close(fd);
    }
    close(listener);
    for(i=0;i<CLIENTS;i++)if(children[i])kill(children[i],SIGTERM);
    for(i=0;i<CLIENTS;i++)if(children[i])while(waitpid(children[i],NULL,0)<0&&errno==EINTR);
    wolfSSH_CTX_free(ctx);wolfSSH_Cleanup();return stopping?0:1;
usage:
    fputs("usage: sshd [-p PORT] [--host-key DER] [--authorized-keys FILE]\n"
          "       sshd --keygen NEW-DER-FILE\n"
          "Foreground SSH server; root public-key login, one channel per connection.\n",stderr);
    return argc==2&&!strcmp(argv[1],"--help")?0:2;
}
