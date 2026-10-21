/* SPDX-License-Identifier: GPL-3.0-or-later */
/* One frontend, invoked as ssh, sftp or scp. All transport stays in the guest. */
#include "keys.h"
#include "ripple_crypto.h"
#include <wolfssh/ssh.h>
#include <wolfssh/wolfsftp.h>
#include <wolfssl/wolfcrypt/coding.h>
#include <wolfssl/wolfcrypt/hmac.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <time.h>
#include <termios.h>
#include <sys/stat.h>
#include <dirent.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netdb.h>
#include "client-config.inc"
static volatile int stopping,resizing;
static unsigned long now(void){struct timespec t;if(clock_gettime(CLOCK_MONOTONIC,&t))return 0;return (unsigned long)t.tv_sec*1000UL+t.tv_nsec/1000000UL;}
static void caught(int sig){stopping=sig;signal(sig,caught);}
static void resized(int sig){resizing=1;signal(sig,resized);}
static int transient(int r){return r==WS_WANT_READ||r==WS_WANT_WRITE||r==WS_REKEYING||r==WS_WINDOW_FULL;}
static int socket_read(WOLFSSH *ssh,void *data,word32 size,void *context) {
    struct connection *c=context;int n;(void)ssh;
    n=read(c->fd,data,size>16384UL?16384:(unsigned)size);
    if(n>0){c->last_io=now();return n;}if(!n)return WS_CBIO_ERR_CONN_CLOSE;
    if(errno==EINTR)return WS_CBIO_ERR_ISR;return errno==EAGAIN?WS_CBIO_ERR_WANT_READ:WS_CBIO_ERR_GENERAL;
}
static int socket_write(WOLFSSH *ssh,void *data,word32 size,void *context) {
    struct connection *c=context;byte *p=data;unsigned i,count=size>16384UL?16384:(unsigned)size;int n;(void)ssh;
    for(i=0;i<count;i++)if(p[i]>255)return WS_CBIO_ERR_GENERAL;
    n=write(c->fd,data,count);if(n>0){c->last_io=now();return n;}if(n<0&&errno==EINTR)return WS_CBIO_ERR_ISR;return n<0&&errno==EAGAIN?WS_CBIO_ERR_WANT_WRITE:WS_CBIO_ERR_GENERAL;
}
static int user_auth(byte type,WS_UserAuthData *auth,void *context) {
    struct connection *c=context;
    if(type!=WOLFSSH_USERAUTH_PUBLICKEY||!c->key.der_length)return WOLFSSH_USERAUTH_FAILURE;
    auth->sf.publicKey.publicKeyType=(byte *)c->key.algorithm;auth->sf.publicKey.publicKeyTypeSz=strlen(c->key.algorithm);
    auth->sf.publicKey.publicKey=c->key.blob;auth->sf.publicKey.publicKeySz=c->key.blob_length;
    auth->sf.publicKey.privateKey=c->key.der;auth->sf.publicKey.privateKeySz=c->key.der_length;
    return WOLFSSH_USERAUTH_SUCCESS;
}
static int wait_socket(struct connection *c,unsigned long started,unsigned long timeout) {
    fd_set reads,writes;struct timeval tv;unsigned long elapsed=now()-started,remain;int r;
    if(stopping){errno=EINTR;return -1;}if(elapsed>=timeout){errno=ETIMEDOUT;return -1;}
    remain=timeout-elapsed;if(remain>100)remain=100;
    FD_ZERO(&reads);FD_ZERO(&writes);FD_SET(c->fd,&reads);
    if(wolfSSH_OutputPending(c->ssh)||wolfSSH_get_error(c->ssh)==WS_WANT_WRITE)FD_SET(c->fd,&writes);
    tv.tv_sec=0;tv.tv_usec=remain*1000;
    r=select(c->fd+1,&reads,&writes,NULL,&tv);return r<0&&errno!=EINTR?-1:0;
}
static int retry(struct connection *c,int32_t r,unsigned long start) {
    int e=wolfSSH_get_error(c->ssh);
    if(transient((int)r)||(r==WS_FATAL_ERROR&&transient(e)))return wait_socket(c,start,120000UL)?-1:1;
    if(r<0){fprintf(stderr,"ssh: protocol failure %ld (%d: %s)\n",(long)r,e,wolfSSH_ErrorToName(e));errno=EIO;return -1;}return 0;
}
static void disconnect(struct connection *c) {
    if(c->ssh) {
        unsigned long start=now();int r;
        do {r=wolfSSH_shutdown(c->ssh);if(r==WS_SUCCESS||r==WS_CHANNEL_CLOSED||!transient(wolfSSH_get_error(c->ssh)))break;if(wait_socket(c,start,3000))break;}while(!stopping);
        wolfSSH_free(c->ssh);
    }
    if(c->ctx)wolfSSH_CTX_free(c->ctx);if(c->fd>=0)close(c->fd);ssh_wipe(&c->key,sizeof(c->key));
}
static int connect_server(struct connection *c,int file_transfer,int terminal,const char *command) {
    struct sockaddr_in address;struct hostent *h;unsigned long start=now();int r,e;
    c->fd=socket(AF_INET,SOCK_STREAM,0);if(c->fd<0)return -1;
    memset(&address,0,sizeof(address));address.sin_family=AF_INET;address.sin_port=htons(c->config.port);
    if(!inet_aton(c->config.host,&address.sin_addr)) {
        h=gethostbyname(c->config.host);if(!h||h->h_addrtype!=AF_INET||h->h_length!=sizeof(address.sin_addr)){errno=EHOSTUNREACH;return -1;}
        memcpy(&address.sin_addr,h->h_addr,sizeof(address.sin_addr));
    }
    if(fcntl(c->fd,F_SETFL,O_NONBLOCK)<0)return -1;
    for(;;) {
        r=connect(c->fd,(void *)&address,sizeof(address));if(!r||errno==EISCONN)break;
        if(errno!=EINPROGRESS&&errno!=EALREADY&&errno!=EINTR)return -1;
        if(now()-start>=(unsigned long)c->config.timeout*1000UL){errno=ETIMEDOUT;return -1;}
        {fd_set write;struct timeval tv;FD_ZERO(&write);FD_SET(c->fd,&write);tv.tv_sec=0;tv.tv_usec=100000;if(select(c->fd+1,NULL,&write,NULL,&tv)<0&&errno!=EINTR)return -1;}
        if(stopping){errno=EINTR;return -1;}
    }
    c->ctx=wolfSSH_CTX_new(WOLFSSH_ENDPOINT_CLIENT,NULL);if(!c->ctx)return -1;
    if(wolfSSH_CTX_SetAlgoListKex(c->ctx,"ecdh-sha2-nistp256,ecdh-sha2-nistp384,ecdh-sha2-nistp521")||
       wolfSSH_CTX_SetAlgoListKey(c->ctx,"ecdsa-sha2-nistp256,ecdsa-sha2-nistp384,ecdsa-sha2-nistp521,ssh-ed25519,rsa-sha2-512,rsa-sha2-256")||
       wolfSSH_CTX_SetAlgoListCipher(c->ctx,"aes128-gcm@openssh.com,aes256-gcm@openssh.com"))return -1;
    wolfSSH_SetUserAuth(c->ctx,user_auth);wolfSSH_CTX_SetPublicKeyCheck(c->ctx,host_key);
    wolfSSH_SetIORecv(c->ctx,socket_read);wolfSSH_SetIOSend(c->ctx,socket_write);
    c->ssh=wolfSSH_new(c->ctx);if(!c->ssh)return -1;
    wolfSSH_set_fd(c->ssh,c->fd);wolfSSH_SetIOReadCtx(c->ssh,c);wolfSSH_SetIOWriteCtx(c->ssh,c);
    wolfSSH_SetPublicKeyCheckCtx(c->ssh,c);wolfSSH_SetUserAuthCtx(c->ssh,c);
    if(wolfSSH_SetUsername(c->ssh,c->config.user))return -1;
    if(file_transfer)r=wolfSSH_SetChannelType(c->ssh,WOLFSSH_SESSION_SUBSYSTEM,(byte *)"sftp",4);
    else {
        r=wolfSSH_SetChannelType(c->ssh,terminal?WOLFSSH_SESSION_TERMINAL:WOLFSSH_SESSION_SHELL,NULL,0);
        if(!r&&command&&*command)r=wolfSSH_SetChannelType(c->ssh,WOLFSSH_SESSION_EXEC,(byte *)command,strlen(command));
    }
    if(r)return -1;
    start=c->last_io=now();
    for(;;) {
        /* ConnectTimeout bounds idle I/O. Crypto and slow JIT cold starts can
         * span several protocol phases; cap the whole handshake at four such
         * intervals as well, so a trickling peer cannot hold it indefinitely. */
        if(now()-start>=(unsigned long)c->config.timeout*4000UL){errno=ETIMEDOUT;return -1;}
        r=file_transfer?(int)wolfSSH_SFTP_connect(c->ssh):wolfSSH_connect(c->ssh);
        if(r==WS_SUCCESS||r==WS_SFTP_COMPLETE)break;
        e=wolfSSH_get_error(c->ssh);
        if(!transient(r)&&!(r==WS_FATAL_ERROR&&transient(e))){fprintf(stderr,"ssh: connection failed: %s (%d, %d)\n",wolfSSH_ErrorToName(e),r,e);errno=e==WS_PUBKEY_REJECTED_E?EACCES:EIO;return -1;}
        if(wait_socket(c,c->last_io,(unsigned long)c->config.timeout*1000UL))return -1;
    }
    if(save_host(c)){perror("ssh: save known_hosts");return -1;}
    ssh_wipe(c->key.der,sizeof(c->key.der));c->key.der_length=0;
    return 0;
}
struct block {byte data[8192];unsigned at,length;};
static int write_block(int fd,struct block *b) {
    int n;if(!b->length)return 0;n=write(fd,b->data+b->at,b->length);
    if(n>0){b->at+=n;b->length-=n;return 1;}return n<0&&(errno==EAGAIN||errno==EINTR)?0:-1;
}
static int terminal_session(struct connection *c,int terminal) {
    struct termios old,raw;struct block input={{0},0,0},output={{0},0,0},errors={{0},0,0};
    int have=0,in_flags=-1,out_flags=-1,err_flags=-1,in_eof=0,eof_sent=0,closed=0,r,n,result=255,progress;word32 id=0;
    unsigned last_columns=0,last_rows=0;unsigned long size_checked=0;
    if(terminal&&tcgetattr(0,&old)==0) {
        raw=old;raw.c_lflag&=~(ICANON|ECHO|ISIG);raw.c_iflag&=~(ICRNL|INLCR|IXON);raw.c_oflag&=~OPOST;raw.c_cc[VMIN]=1;raw.c_cc[VTIME]=0;if(tcsetattr(0,TCSANOW,&raw)<0)goto done;have=1;resizing=1;
    }
    in_flags=fcntl(0,F_GETFL,0);out_flags=fcntl(1,F_GETFL,0);err_flags=fcntl(2,F_GETFL,0);
    if(in_flags<0||out_flags<0||err_flags<0||fcntl(0,F_SETFL,in_flags|O_NONBLOCK)<0||fcntl(1,F_SETFL,out_flags|O_NONBLOCK)<0||fcntl(2,F_SETFL,err_flags|O_NONBLOCK)<0)goto done;
    while(!stopping) {
        progress=0;
        if(!closed) {
            r=wolfSSH_worker(c->ssh,&id);
            if(r==WS_CHANNEL_CLOSED||r==WS_DISCONNECT||(r==WS_FATAL_ERROR&&wolfSSH_get_error(c->ssh)==WS_DISCONNECT))closed=1;
            else if(r<0&&r!=WS_CHAN_RXD&&r!=WS_EXTDATA&&r!=WS_EOF&&!transient(r)&&!(r==WS_FATAL_ERROR&&transient(wolfSSH_get_error(c->ssh))))goto done;
        }
        if(!output.length) {
            n=wolfSSH_ChannelIdRead(c->ssh,0,output.data,sizeof(output.data));
            if(n>0){output.at=0;output.length=n;progress=1;}
        }
        if(!errors.length) {
            n=wolfSSH_ChannelIdReadExt(c->ssh,0,errors.data,sizeof(errors.data));
            if(n>0){errors.at=0;errors.length=n;progress=1;}
        }
        n=write_block(1,&output);if(n<0)goto done;progress|=n;n=write_block(2,&errors);if(n<0)goto done;progress|=n;
        if(closed&&!output.length&&!errors.length){result=wolfSSH_GetExitStatus(c->ssh);if(result<0||result>255)result=255;break;}
        if(!input.length&&!in_eof&&!closed) {
            n=read(0,input.data,sizeof(input.data));if(n>0){input.at=0;input.length=n;progress=1;}else if(!n)in_eof=1;else if(errno!=EAGAIN&&errno!=EINTR)goto done;
        }
        if(input.length&&!closed) {
            n=wolfSSH_stream_send(c->ssh,input.data+input.at,input.length);
            if(n>0){input.at+=n;input.length-=n;progress=1;}
            else if(!transient(n)&&!(n==WS_FATAL_ERROR&&transient(wolfSSH_get_error(c->ssh))))goto done;
        }
        if(in_eof&&!input.length&&!eof_sent&&!closed) {
            r=wolfSSH_stream_send_eof(c->ssh);if(r==WS_SUCCESS||r==WS_WANT_WRITE)eof_sent=1;else if(!transient(r))goto done;
        }
        /* Physical consoles discover host changes during TIOCGWINSZ. PTYs
         * signal them; poll occasionally too so both console paths resize. */
        if(terminal&&!closed&&(resizing||now()-size_checked>=250UL)) {
            struct winsize w;resizing=0;size_checked=now();
            if(!ioctl(0,TIOCGWINSZ,&w)&&w.ws_col&&w.ws_row&&(w.ws_col!=last_columns||w.ws_row!=last_rows)) {
                r=wolfSSH_ChangeTerminalSize(c->ssh,w.ws_col,w.ws_row,w.ws_xpixel,w.ws_ypixel);
                if(r==WS_SUCCESS||r==WS_WANT_WRITE){last_columns=w.ws_col;last_rows=w.ws_row;}
                else if(transient(r))resizing=1;else if(r<0)goto done;
            }
        }
        if(!progress) {
            fd_set reads,writes;struct timeval tv;int max=c->fd;
            FD_ZERO(&reads);FD_ZERO(&writes);if(!closed)FD_SET(c->fd,&reads);
            if(wolfSSH_OutputPending(c->ssh))FD_SET(c->fd,&writes);
            if(!in_eof&&!input.length&&!closed){FD_SET(0,&reads);if(max<0)max=0;}
            if(output.length){FD_SET(1,&writes);if(max<1)max=1;}
            if(errors.length){FD_SET(2,&writes);if(max<2)max=2;}
            tv.tv_sec=0;tv.tv_usec=100000;if(select(max+1,&reads,&writes,NULL,&tv)<0&&errno!=EINTR)goto done;
        }
    }
    if(stopping)result=128+stopping;
done:
    if(have)tcsetattr(0,TCSANOW,&old);
    if(in_flags>=0)fcntl(0,F_SETFL,in_flags);if(out_flags>=0)fcntl(1,F_SETFL,out_flags);if(err_flags>=0)fcntl(2,F_SETFL,err_flags);
    return result;
}
#include "client-files.inc"
int main(int argc,char **argv) {
    struct connection *c=calloc(1,sizeof(*c));char dir[SSH_PATH],config[SSH_PATH],command[2048],endpoint[SSH_PATH];
    const char *program=strrchr(argv[0],'/'),*configuration=NULL,*batchfile=NULL;char **args;unsigned i=1,at=0;int mode,r=255,terminal=0,explicit_config=0,recursive=0;
    if(!c)return 255;c->fd=-1;program=program?program+1:argv[0];mode=!strncmp(program,"sftp",4)?1:!strncmp(program,"scp",3)?2:0;
    c->config.port=22;c->config.timeout=30;
    while(i<(unsigned)argc&&argv[i][0]=='-'&&argv[i][1]) {
        const char *opt=argv[i++];if(!strcmp(opt,"--"))break;
        if(!strcmp(opt,"-t")){c->config.tty=2;c->config.seen|=OPT_TTY;continue;}
        if(!strcmp(opt,"-T")){c->config.tty=1;c->config.seen|=OPT_TTY;continue;}
        if(!strcmp(opt,"-q"))continue;
        if(!strcmp(opt,"-r")&&mode==2){recursive=1;continue;}
        if(i>=(unsigned)argc)goto usage;
        if(!strcmp(opt,"-o")){if(command_option(&c->config,argv[i++]))goto done;}
        else if(!strcmp(opt,"-i")){if(option(&c->config,"IdentityFile",argv[i++]))goto done;}
        else if(!strcmp(opt,"-p")&&mode==0||!strcmp(opt,"-P")&&mode!=0){if(option(&c->config,"Port",argv[i++]))goto done;}
        else if(!strcmp(opt,"-l")&&mode==0){if(option(&c->config,"User",argv[i++]))goto done;}
        else if(!strcmp(opt,"-F")){configuration=argv[i++];explicit_config=1;}
        else if(!strcmp(opt,"-b")&&mode==1){batchfile=argv[i++];c->config.batch=1;c->config.seen|=OPT_BATCH;}
        else goto usage;
    }
    args=argv+i;if(i>=(unsigned)argc)goto usage;
    if(mode==2) {
        char *remote;int src;
        if(argc-i!=2)goto usage;
        src=remote_name(args[0],endpoint,sizeof(endpoint),&remote);
        if(src<0)goto usage;
        if(!src){if(remote_name(args[1],endpoint,sizeof(endpoint),&remote)!=1)goto usage;}
    } else if(copy_text(endpoint,sizeof(endpoint),args[0]))goto done;
    {char *user=strchr(endpoint,'@');if(user){*user++=0;if(!(c->config.seen&OPT_USER)&&option(&c->config,"User",endpoint))goto done;memmove(endpoint,user,strlen(user)+1);}}
    if(!*endpoint||strchr(endpoint,':')||strchr(endpoint,'/')||copy_text(c->config.alias,sizeof(c->config.alias),endpoint))goto usage;
    {unsigned j;for(j=0;endpoint[j];j++)if((unsigned)endpoint[j]<33||(unsigned)endpoint[j]>126)goto usage;}
    if(ssh_directory(dir,sizeof(dir))||ssh_path(config,sizeof(config),"config"))goto failed;
    if(!configuration)configuration=config;
    if(strcmp(configuration,"/dev/null")&&load_config(&c->config,configuration,explicit_config))goto failed;
    if(!c->config.host[0])copy_text(c->config.host,sizeof(c->config.host),endpoint);
    if(!c->config.user[0]){const char *user=getenv("USER");copy_text(c->config.user,sizeof(c->config.user),user?user:"root");}
    if(!c->config.identity[0]) {
        const char *names[]={"id_ecdsa","id_ed25519","id_rsa"};struct stat st;unsigned j;
        for(j=0;j<3;j++){if(ssh_path(c->config.identity,sizeof(c->config.identity),names[j]))goto failed;if(lstat(c->config.identity,&st)==0)break;if(errno!=ENOENT)goto failed;}
    }
    if(!c->config.known[0]&&ssh_path(c->config.known,sizeof(c->config.known),"known_hosts"))goto failed;
    if(wolfSSH_Init()!=WS_SUCCESS)goto failed;ripple_crypto_init();
    if(ssh_key_load(&c->key,c->config.identity,c->config.batch?"":NULL)){fprintf(stderr,"ssh: cannot load identity %s\n",c->config.identity);goto failed;}
    command[0]=0;
    if(!mode) {
        for(i=i+1;i<(unsigned)argc;i++) {unsigned len=strlen(argv[i]);if(at+len+2>sizeof(command))goto usage;if(at)command[at++]=' ';memcpy(command+at,argv[i],len);at+=len;command[at]=0;}
        terminal=c->config.tty==2||(!command[0]&&c->config.tty==0&&isatty(0));
    }
    signal(SIGPIPE,SIG_IGN);signal(SIGINT,caught);signal(SIGTERM,caught);signal(SIGHUP,caught);signal(SIGWINCH,resized);
    if(connect_server(c,mode!=0,terminal,command))goto failed;
    r=mode==0?terminal_session(c,terminal):mode==1?sftp_session(c,batchfile):scp_session(c,args[0],args[1],recursive);
    goto done;
failed:perror(program);goto done;
usage:
    if(!mode)fputs("usage: ssh [-t|-T] [-p port] [-l user] [-i key] [-F config] [-o option=value] [user@]host [command ...]\n",stderr);
    else if(mode==1)fputs("usage: sftp [-P port] [-i key] [-F config] [-o option=value] [-b batch] [user@]host\n",stderr);
    else fputs("usage: scp [-r] [-P port] [-i key] [-F config] [-o option=value] source [user@]host:path\n       scp [options] [user@]host:path destination\n",stderr);
    r=2;
done:disconnect(c);ssh_wipe(c,sizeof(*c));free(c);wolfSSH_Cleanup();return r;
}
