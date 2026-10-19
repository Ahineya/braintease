/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Real-kernel tests, shared by Ripple/JIT and native Fuse-X. */
#include <sys/select.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <net/if.h>
#include <sys/wait.h>
#include <sys/ioctl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <termios.h>
#include <time.h>
#include <ripple_net_limits.h>
static fd_set r,w,x;
static char data[1024];
static volatile int caught;
static int capacity_stress;
extern long call(unsigned,long,long,long,long);
static void fail(unsigned line){printf("selectcheck FAILED line %u errno %d\n",line,errno);exit(1);}
#define C(a) do { if(!(a))fail(__LINE__); } while(0)
static void handler(int s){caught++;signal(s,handler);}
static void delay(unsigned usec){struct timeval t={0,0};t.tv_usec=usec;C(select(0,NULL,NULL,NULL,&t)==0);}
static int pollfd(int fd,int writing){struct timeval t={0,0};FD_ZERO(&r);FD_SET(fd,&r);return select(fd+1,writing?NULL:&r,writing?&r:NULL,NULL,&t);}
static int waitfd(int fd,int writing){struct timeval t={5,0};FD_ZERO(&r);FD_SET(fd,&r);return select(fd+1,writing?NULL:&r,writing?&r:NULL,NULL,&t);}
static void waited(int pid){int s;C(waitpid(pid,&s,0)==pid);C(WIFEXITED(s)&&WEXITSTATUS(s)==0);}
static void api(void){
    int fd,high,n;char *allocation,*edge;struct timeval t={0,0};struct timespec before,after;unsigned long ms;
    struct ripple_select_request req;
    C(sizeof(fd_set)==2&&sizeof(struct timeval)==4&&sizeof(req)==9);
    memset(&req,0,sizeof(req));req.version=RIPPLE_SELECT_VERSION+1;
    C(call(72,0,(long)&req,0,0)==-1&&errno==EINVAL);
    req.version=RIPPLE_SELECT_VERSION;
    C(call(72,65536UL,(long)&req,0,0)==-1&&errno==EINVAL);
    FD_ZERO(&r);FD_SET(0,&r);FD_SET(15,&r);FD_SET(16,&r);FD_SET(31,&r);
    C(r.bits[0]==0x8001&&r.bits[1]==0x8001);FD_CLR(16,&r);C(!FD_ISSET(16,&r)&&FD_ISSET(31,&r));
    C(select(-1,NULL,NULL,NULL,&t)==-1&&errno==EINVAL);
    C(select(33,NULL,NULL,NULL,&t)==-1&&errno==EINVAL);
    FD_ZERO(&r);FD_SET(14,&r);C(select(15,&r,NULL,NULL,&t)==-1&&errno==EBADF);
    FD_ZERO(&r);FD_SET(31,&r);C(select(0,&r,NULL,NULL,&t)==0&&!r.bits[0]&&!r.bits[1]);
    t.tv_usec=1000000UL;C(select(0,NULL,NULL,NULL,&t)==-1&&errno==EINVAL);
    t.tv_usec=(unsigned long)-1;C(select(0,NULL,NULL,NULL,&t)==-1&&errno==EINVAL);
    t.tv_usec=0;t.tv_sec=(unsigned long)-1;C(select(0,NULL,NULL,NULL,&t)==-1&&errno==EINVAL);
    t.tv_sec=0;
    C(select(0,(fd_set *)0x60000UL,NULL,NULL,&t)==-1&&errno==EFAULT);
    C(select(0,NULL,NULL,NULL,(void *)0x60000UL)==-1&&errno==EFAULT);
    fd=open("/etc/passwd",O_RDONLY);C(fd>=0);
    for(n=15;n<32;n++) {
        if(n!=15&&n!=16&&n!=31)continue;
        high=dup2(fd,n);C(high==n);FD_ZERO(&r);FD_ZERO(&w);FD_ZERO(&x);
        FD_SET(fd,&r);FD_SET(n,&r);FD_SET(n,&w);FD_SET(n,&x);
        C(select(n+1,&r,&w,&x,&t)==3);C(FD_ISSET(fd,&r)&&FD_ISSET(n,&r)&&FD_ISSET(n,&w)&&!FD_ISSET(n,&x));
        C(lseek(n,0,2)>=0&&pollfd(n,0)==1);C(read(n,data,1)==0);close(n);
    }
    allocation=malloc(70000UL);C(allocation!=NULL);
    edge=allocation+(64000UL-((unsigned long)allocation&65535UL))-1UL;
    FD_ZERO((fd_set *)edge);FD_SET(fd,(fd_set *)edge);
    C(select(fd+1,(fd_set *)edge,NULL,NULL,&t)==1&&FD_ISSET(fd,(fd_set *)edge));
    t.tv_sec=0;t.tv_usec=0;memcpy(edge,&t,sizeof(t));
    C(select(0,NULL,NULL,NULL,(struct timeval *)edge)==0);
    free(allocation);close(fd);
    C(clock_gettime(CLOCK_MONOTONIC,&before)==0);t.tv_usec=200000UL;
    C(select(0,NULL,NULL,NULL,&t)==0&&t.tv_usec==200000UL&&t.tv_sec==0);
    C(clock_gettime(CLOCK_MONOTONIC,&after)==0);
    ms=(after.tv_sec-before.tv_sec)*1000UL+(after.tv_nsec-before.tv_nsec)/1000000L;C(ms>=200&&ms<3000);
    t.tv_usec=1;C(select(0,NULL,NULL,NULL,&t)==0&&t.tv_usec==1);
    puts("selectcheck: API, masks, high FDs, EFAULT, EOF and deadlines passed");
}
static void pipes(void){
    int p[2],pid,i,n,other,kids[2];unsigned total;char byte;
    C(pipe(p)==0);C(pollfd(p[0],0)==0&&pollfd(p[1],1)==1);
    C(fcntl(p[1],F_SETFL,O_NONBLOCK)==0);memset(data,'p',sizeof(data));
    total=0;while((n=write(p[1],data,sizeof(data)))>0)total+=n;
    C(total==8192UL&&n==-1&&errno==EAGAIN&&pollfd(p[1],1)==0);
    pid=fork();C(pid>=0);
    if(!pid){close(p[1]);delay(200000);_exit(read(p[0],data,512)==512?0:81);}
    C(waitfd(p[1],1)==1);waited(pid);C(read(p[0],data,1024)==1024);
    close(p[0]);C(pollfd(p[1],1)==1);signal(SIGPIPE,SIG_IGN);
    C(write(p[1],"x",1)==-1&&errno==EPIPE);close(p[1]);
    C(pipe(p)==0);other=dup(p[1]);close(p[1]);C(pollfd(p[0],0)==0);close(other);
    C(pollfd(p[0],0)==1&&read(p[0],&byte,1)==0);close(p[0]);
    for(i=0;i<8;i++) {
        C(pipe(p)==0);pid=fork();C(pid>=0);
        if(!pid){close(p[0]);delay(100000);_exit(write(p[1],"x",1)==1?0:82);}
        close(p[1]);FD_ZERO(&r);FD_SET(p[0],&r);
        C(select(p[0]+1,&r,NULL,NULL,NULL)==1&&FD_ISSET(p[0],&r));
        C(read(p[0],&byte,1)==1&&byte=='x');waited(pid);close(p[0]);
    }
    /* Independent selectors on the same inode; neither consumes the byte. */
    C(pipe(p)==0);
    for(i=0;i<2;i++){kids[i]=fork();C(kids[i]>=0);if(!kids[i]){close(p[1]);_exit(waitfd(p[0],0)==1?0:83);}}
    delay(200000);C(write(p[1],"x",1)==1);waited(kids[0]);waited(kids[1]);close(p[0]);close(p[1]);
    puts("selectcheck: pipe data, backpressure, dup/fork, EOF and broken pipe passed");
}
static void signals(void){
    int p[2],pid,i;struct timeval t;struct timespec a,b;long ms;
    signal(SIGUSR1,handler);
    for(i=0;i<4;i++) {
        C(pipe(p)==0);pid=fork();C(pid>=0);
        if(!pid){delay(100000);kill(getppid(),SIGUSR1);_exit(0);}
        FD_ZERO(&r);FD_SET(p[0],&r);t.tv_sec=0x7fffffffUL;t.tv_usec=999999;
        C(select(p[0]+1,&r,NULL,NULL,&t)==-1&&errno==EINTR);waited(pid);close(p[0]);close(p[1]);
    }
    C(caught==4);
    /* Ignored SIGCONT is a spurious wake, not a deadline reset or EINTR. */
    pid=fork();C(pid>=0);if(!pid){delay(100000);kill(getppid(),SIGCONT);_exit(0);}
    t.tv_sec=0;t.tv_usec=600000;clock_gettime(CLOCK_MONOTONIC,&a);
    C(select(0,NULL,NULL,NULL,&t)==0);clock_gettime(CLOCK_MONOTONIC,&b);
    ms=(b.tv_sec-a.tv_sec)*1000L+(b.tv_nsec-a.tv_nsec)/1000000L;C(ms>=600&&ms<3000);waited(pid);
    pid=fork();C(pid>=0);if(!pid){time_t future;delay(100000);future=time(NULL)+3600;_exit(stime(&future)==0?0:88);}
    t.tv_sec=0;t.tv_usec=400000;clock_gettime(CLOCK_MONOTONIC,&a);
    C(select(0,NULL,NULL,NULL,&t)==0);clock_gettime(CLOCK_MONOTONIC,&b);
    ms=(b.tv_sec-a.tv_sec)*1000L+(b.tv_nsec-a.tv_nsec)/1000000L;C(ms>=400&&ms<3000);waited(pid);
    signal(SIGALRM,handler);alarm(1);t.tv_sec=10;t.tv_usec=0;
    C(select(0,NULL,NULL,NULL,&t)==-1&&errno==EINTR&&caught==5);alarm(0);
    /* Killing a blocked selector must leave no bits attached to reused slots. */
    for(i=0;i<4;i++) {
        C(pipe(p)==0);pid=fork();C(pid>=0);
        if(!pid){close(p[1]);FD_ZERO(&r);FD_SET(p[0],&r);select(p[0]+1,&r,NULL,NULL,NULL);_exit(84);}
        delay(100000);C(kill(pid,SIGKILL)==0);C(waitpid(pid,NULL,0)==pid);close(p[0]);close(p[1]);
    }
    puts("selectcheck: EINTR, alarms, long waits, spurious wakes and killed waiter reuse passed");
}
static void terminals(void){
    int master,slave,pid,i;struct termios t;char byte;
    master=open("/dev/ptyp0",O_RDWR|O_NONBLOCK|O_NOCTTY);C(master>=0);
    slave=open("/dev/ttyp0",O_RDWR|O_NOCTTY);C(slave>=0);C(tcgetattr(slave,&t)==0);
    t.c_lflag|=ICANON;t.c_lflag&=~ECHO;C(tcsetattr(slave,TCSANOW,&t)==0);
    C(write(master,"a",1)==1&&pollfd(slave,0)==0);C(write(master,"\n",1)==1&&pollfd(slave,0)==1);
    C(read(slave,data,10)==2);byte=t.c_cc[VEOF];C(write(master,&byte,1)==1&&pollfd(slave,0)==1);C(read(slave,data,1)==0);
    t.c_lflag&=~ICANON;t.c_cc[VMIN]=2;t.c_cc[VTIME]=0;C(tcsetattr(slave,TCSANOW,&t)==0);
    C(write(master,"b",1)==1&&pollfd(slave,0)==0);C(write(master,"c",1)==1&&pollfd(slave,0)==1);C(read(slave,data,2)==2);
    t.c_cc[VTIME]=1;C(tcsetattr(slave,TCSANOW,&t)==0);C(pollfd(slave,0)==0);
    C(write(master,"b",1)==1&&pollfd(slave,0)==1);C(read(slave,data,1)==1);
    t.c_cc[VMIN]=0;t.c_cc[VTIME]=0;C(tcsetattr(slave,TCSANOW,&t)==0);C(pollfd(slave,0)==1&&read(slave,data,1)==0);
    t.c_cc[VTIME]=1;C(tcsetattr(slave,TCSANOW,&t)==0);C(pollfd(slave,0)==0);
    t.c_cc[VMIN]=1;t.c_cc[VTIME]=0;C(tcsetattr(slave,TCSANOW,&t)==0);
    pid=fork();C(pid>=0);if(!pid){delay(100000);_exit(write(master,"q",1)==1?0:85);}
    C(waitfd(slave,0)==1&&read(slave,&byte,1)==1&&byte=='q');waited(pid);
    /* Fill slave input: consuming it wakes a master waiting to write. */
    for(i=0;i<512;i++)C(write(master,"x",1)==1);C(pollfd(master,1)==0);
    pid=fork();C(pid>=0);if(!pid){delay(100000);_exit(read(slave,data,128)==128?0:86);}
    C(waitfd(master,1)==1);waited(pid);C(read(slave,data,384)==384);
    /* Master output readiness and slave output space. */
    C(fcntl(slave,F_SETFL,O_NONBLOCK)==0);memset(data,'y',512);
    C(write(slave,data,512)==512);
    C(pollfd(slave,1)==0&&pollfd(master,0)==1);
    C(read(master,data,128)==128&&pollfd(slave,1)==1);
    C(read(master,data,384)==384);
    close(slave);C(pollfd(master,0)==1&&read(master,&byte,1)==0);close(master);
    master=open("/dev/ptyp0",O_RDWR|O_NOCTTY);slave=open("/dev/ttyp0",O_RDWR|O_NOCTTY);C(master>=0&&slave>=0);
    close(master);C(pollfd(slave,0)==1&&pollfd(slave,1)==1);close(slave);
    puts("selectcheck: canonical/EOF, VMIN/VTIME, PTY read/write queues and hangups passed");
}
static void network(void){
    struct sockaddr_in peer,local;int fd,n,i,so,pid,listenfd;unsigned sent=0,received=0;char reply[900];socklen_t len;
    memset(&peer,0,sizeof(peer));peer.sin_family=AF_INET;peer.sin_port=htons(7001);C(inet_pton(AF_INET,"192.168.64.1",&peer.sin_addr)==1);
    fd=socket(AF_INET,SOCK_STREAM,0);C(fd>=0&&fcntl(fd,F_SETFL,O_NONBLOCK)==0);
    C(connect(fd,(void *)&peer,sizeof(peer))==-1&&errno==EINPROGRESS);C(waitfd(fd,1)==1);
    len=sizeof(so);C(getsockopt(fd,SOL_SOCKET,SO_ERROR,&so,&len)==0&&so==0&&len==sizeof(so));
    for(i=0;i<900;i++)data[i]=i&255;
    while(sent<900){C(waitfd(fd,1)==1);n=write(fd,data+sent,900-sent);if(n<0&&errno==EAGAIN)continue;C(n>0);sent+=n;}
    C(shutdown(fd,SHUT_WR)==0&&pollfd(fd,1)==1);
    while(received<900){C(waitfd(fd,0)==1);n=read(fd,reply+received,900-received);if(n<0&&errno==EAGAIN)continue;C(n>0);received+=n;}
    C(memcmp(data,reply,900)==0);C(waitfd(fd,0)==1&&read(fd,data,1)==0);close(fd);
    peer.sin_port=htons(7002);fd=socket(AF_INET,SOCK_STREAM,0);C(fd>=0&&fcntl(fd,F_SETFL,O_NONBLOCK)==0);
    C(connect(fd,(void *)&peer,sizeof(peer))==-1&&errno==EINPROGRESS);C(waitfd(fd,1)==1);
    len=sizeof(so);C(getsockopt(fd,SOL_SOCKET,SO_ERROR,&so,&len)==0&&so==ECONNREFUSED);
    len=sizeof(so);C(getsockopt(fd,SOL_SOCKET,SO_ERROR,&so,&len)==0&&so==0);close(fd);
    fd=socket(AF_INET,SOCK_DGRAM,0);C(fd>=0&&fcntl(fd,F_SETFL,O_NONBLOCK)==0);peer.sin_port=htons(7000);
    C(connect(fd,(void *)&peer,sizeof(peer))==0&&pollfd(fd,0)==0);
    C(waitfd(fd,1)==1);C(send(fd,"",0,0)==0);C(waitfd(fd,0)==1&&recv(fd,data,1,0)==0);
    C(waitfd(fd,1)==1&&write(fd,"udp",3)==3);C(waitfd(fd,0)==1&&read(fd,data,3)==3&&!memcmp(data,"udp",3));close(fd);
    puts("selectcheck: asynchronous connect, SO_ERROR, TCP streams/EOF and UDP passed");
    listenfd=socket(AF_INET,SOCK_STREAM,0);C(listenfd>=0);memset(&local,0,sizeof(local));local.sin_family=AF_INET;local.sin_port=htons(7331);
    C(bind(listenfd,(void *)&local,sizeof(local))==0&&listen(listenfd,1)==0);C(pollfd(listenfd,0)==0&&pollfd(listenfd,1)==0);
    puts("SELECT_LISTEN_READY");fflush(stdout);C(waitfd(listenfd,0)==1);
    fd=accept(listenfd,NULL,NULL);C(fd>=0);C(waitfd(fd,0)==1&&read(fd,data,1)==1&&data[0]=='L');
    puts("SELECT_RESET_READY");fflush(stdout);
    C(waitfd(fd,0)==1&&read(fd,data,1)==-1&&errno==ECONNRESET);close(fd);close(listenfd);
    fd=socket(AF_INET,SOCK_DGRAM,0);C(fd>=0);pid=fork();C(pid>=0);
    if(!pid){int ctl;struct ifreq f;delay(200000);ctl=socket(AF_INET,SOCK_DGRAM,0);memset(&f,0,sizeof(f));strcpy(f.ifr_name,"eth0");_exit(ioctl(ctl,SIOCSIFFLAGS,&f)==0?0:87);}
    C(waitfd(fd,0)==1&&read(fd,data,1)==-1&&errno==ENETDOWN);waited(pid);close(fd);
    puts("selectcheck: listener accept, reset and network loss passed");
}
/* Four processes keep the per-process descriptor ceiling independent from
 * the system socket ceiling. No connections are inherited across fork. */
static void capacity_group(unsigned group,int network,int ready,int release){
    struct sockaddr_in a;int fds[16],i,n;char byte,reply[64],pattern[64];
    memset(&a,0,sizeof(a));a.sin_family=AF_INET;a.sin_port=htons(7001);
    C(inet_pton(AF_INET,"192.168.64.1",&a.sin_addr)==1);
    for(i=0;i<16;i++) {
        fds[i]=socket(AF_INET,SOCK_STREAM,0);C(fds[i]>=0);
        if(network) {
            C(fcntl(fds[i],F_SETFL,O_NONBLOCK)==0);
            C(connect(fds[i],(void *)&a,sizeof(a))==-1&&errno==EINPROGRESS&&waitfd(fds[i],1)==1);
            for(n=0;n<64;n++)pattern[n]=(group*16+i+n)&255;
            C(write(fds[i],pattern,64)==64);
            C(waitfd(fds[i],0)==1); /* Leave distinct data queued across memory pressure. */
        }
    }
    if(group) {C(write(ready,"r",1)==1&&read(release,&byte,1)==1);}
    else {
        for(i=0;i<3;i++)C(read(ready,&byte,1)==1);
        C(socket(AF_INET,SOCK_STREAM,0)==-1&&errno==ENOBUFS);
        if(network&&capacity_stress) {
            int pid=fork();C(pid>=0);
            if(!pid) {
                char *args[]={"/bin/mmucheck","--stress",NULL};
                execv(args[0],args);_exit(89);
            }
            waited(pid);
            puts("CAPACITY_MEMORY_PRESSURE_OK");
        }
        C(write(release,"ddd",3)==3);
    }
    for(i=0;i<16;i++) {
        if(network) {
            C(waitfd(fds[i],0)==1&&read(fds[i],reply,64)==64);
            for(n=0;n<64;n++)C((unsigned char)reply[n]==((group*16+i+n)&255));
            C(shutdown(fds[i],SHUT_WR)==0&&waitfd(fds[i],0)==1&&read(fds[i],&byte,1)==0);
        }
        close(fds[i]);
    }
}
static void capacity(void){
    struct sockaddr_in a;int listeners[RIPPLE_NET_TCP_LISTENERS],p[2],q[2],kids[3],pid,i,j,fd;
    char byte;unsigned pass;
    C(RIPPLE_NET_SOCKETS==64&&RIPPLE_NET_TCP_CONNECTIONS==64);
    memset(&a,0,sizeof(a));a.sin_family=AF_INET;
    for(i=0;i<RIPPLE_NET_TCP_LISTENERS;i++) {
        listeners[i]=socket(AF_INET,SOCK_STREAM,0);C(listeners[i]>=0);a.sin_port=htons(7400+i);
        C(bind(listeners[i],(void *)&a,sizeof(a))==0&&listen(listeners[i],4)==0);
    }
    fd=socket(AF_INET,SOCK_STREAM,0);C(fd>=0);a.sin_port=htons(7490);
    C(bind(fd,(void *)&a,sizeof(a))==0&&listen(fd,1)==-1&&errno==ENOBUFS);close(fd);
    puts("SELECT_CAPACITY_LISTENERS_READY");fflush(stdout);
    for(i=0;i<RIPPLE_NET_TCP_LISTENERS;i++) {
        C(waitfd(listeners[i],0)==1);fd=accept(listeners[i],NULL,NULL);C(fd>=0);
        C(waitfd(fd,0)==1&&read(fd,&byte,1)==1&&(unsigned char)byte==i);
        C(write(fd,&byte,1)==1&&shutdown(fd,SHUT_WR)==0);
        C(waitfd(fd,0)==1&&read(fd,&byte,1)==0);close(fd);close(listeners[i]);
    }
    delay(100000);
    /* Exercise all sixteen independently bound UDP ports and binary queues. */
    for(i=0;i<RIPPLE_NET_UDP_PORTS;i++) {
        listeners[i]=socket(AF_INET,SOCK_DGRAM,0);C(listeners[i]>=0);
        a.sin_addr.s_addr=0;a.sin_port=htons(7500+i);
        C(bind(listeners[i],(void *)&a,sizeof(a))==0);
    }
    C(inet_pton(AF_INET,"192.168.64.1",&a.sin_addr)==1);a.sin_port=htons(7000);
    for(i=0;i<RIPPLE_NET_UDP_PORTS;i++) {
        byte=i;C(sendto(listeners[i],&byte,1,0,(void *)&a,sizeof(a))==1);
    }
    for(i=0;i<RIPPLE_NET_UDP_PORTS;i++) {
        C(waitfd(listeners[i],0)==1&&read(listeners[i],&byte,1)==1&&(unsigned char)byte==i);
        close(listeners[i]);
    }
    /* Verify sixty-four live streams, then reuse every endpoint after close. */
    for(pass=0;pass<2;pass++) {
        C(pipe(p)==0&&pipe(q)==0);
        for(i=0;i<3;i++) {
            pid=fork();C(pid>=0);
            if(!pid) {
                close(p[0]);close(q[1]);
                capacity_group(i+1,!pass,p[1],q[0]);_exit(0);
            }
            kids[i]=pid;
        }
        close(p[1]);close(q[0]);capacity_group(0,!pass,p[0],q[1]);
        for(j=0;j<3;j++)waited(kids[j]);close(p[0]);close(q[1]);
        delay(100000);
    }
    puts("selectcheck: 16 listeners, 16 UDP ports, 64 live TCP streams, exhaustion and reuse passed");
    puts("SELECT_CAPACITY_OK");
}

int main(int argc,char **argv){
    if(argc>1&&!strcmp(argv[1],"capacity-stress")){capacity_stress=1;capacity();return 0;}
    if(argc>1&&!strcmp(argv[1],"capacity")){capacity();return 0;}
    if(argc>1&&!strcmp(argv[1],"network")){network();puts("SELECT_NETWORK_OK");return 0;}
    if(argc>1&&!strcmp(argv[1],"input")){char c;puts("SELECT_INPUT_READY");fflush(stdout);C(waitfd(0,0)==1&&read(0,&c,1)==1&&c=='k');puts("SELECT_INPUT_OK");return 0;}
    if(argc>1&&!strcmp(argv[1],"idle")){struct timeval t={2,0};puts("SELECT_IDLE_READY");fflush(stdout);C(select(0,NULL,NULL,NULL,&t)==0);puts("SELECT_IDLE_OK");return 0;}
    api();pipes();signals();terminals();puts("SELECT_OK");return 0;
}
