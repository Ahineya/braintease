/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Real-kernel local IPC checks. Run with no NIC on both execution backends. */
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static char data[2048],copy[2048];
static volatile unsigned caught;
static void fail(unsigned line){printf("localcheck FAILED line %u errno %d\n",line,errno);exit(1);}
#define C(a) do{if(!(a))fail(__LINE__);}while(0)
static void signal_handler(int sig){caught++;signal(sig,signal_handler);}
static void delay(void){struct timeval t={0,200000};C(select(0,0,0,0,&t)==0);}
static void waited(int pid){int s;C(waitpid(pid,&s,0)==pid&&WIFEXITED(s)&&WEXITSTATUS(s)==0);}
static int ready(int fd,int writing,int wait){fd_set set;struct timeval t={0,0};FD_ZERO(&set);FD_SET(fd,&set);t.tv_sec=wait?5:0;return select(fd+1,writing?0:&set,writing?&set:0,0,&t);}
static int sock(void){int fd=socket(AF_UNIX,SOCK_STREAM,0);C(fd>=0);return fd;}
static struct sockaddr_un address;
static void path(const char *name){memset(&address,0,sizeof(address));address.sun_family=AF_UNIX;strcpy(address.sun_path,name);}
static int connect_to(int fd){return connect(fd,(void *)&address,sizeof(address));}
static int listener(const char *name,int backlog){int fd=sock();path(name);unlink(name);C(bind(fd,(void *)&address,sizeof(address))==0);C(listen(fd,backlog)==0);return fd;}
static void pairs(void){
    int p[2],i,pid,alias;struct sockaddr_un a;socklen_t length;
    C(socket(AF_UNIX,SOCK_DGRAM,0)==-1&&errno==EPROTONOSUPPORT);
    C(socketpair(AF_UNIX,SOCK_STREAM,0,(void *)0x180000UL)==-1&&errno==EFAULT);
    for(i=0;i<40;i++){C(socketpair(AF_LOCAL,SOCK_STREAM,0,p)==0);close(p[0]);close(p[1]);}
    C(socketpair(AF_UNIX,SOCK_STREAM,0,p)==0);
    length=sizeof(a);C(getsockname(p[0],(void *)&a,&length)==0&&length==1&&a.sun_family==AF_UNIX);
    length=sizeof(a);C(getpeername(p[0],(void *)&a,&length)==0&&length==1);
    C(ready(p[0],0,0)==0&&ready(p[0],1,0)==1);
    for(i=0;i<2048;i++)data[i]=(i*47)^0xA500; /* Full C16 cells, not octet-only TCP. */
    C(write(p[0],data,1024)==1024);
    C(read(p[1],(void *)0x180000UL,1)==-1&&errno==EFAULT);
    C(read(p[1],copy,1024)==1024&&!memcmp(data,copy,1024));
    C(fcntl(p[0],F_SETFL,O_NONBLOCK)==0);
    C(write(p[0],(void *)0x180000UL,1)==-1&&errno==EFAULT);
    C(write(p[0],data,2048)==1024&&ready(p[0],1,0)==0);
    C(write(p[0],data,1)==-1&&errno==EAGAIN);
    pid=fork();C(pid>=0);
    if(!pid){close(p[0]);delay();_exit(read(p[1],copy,128)==128&&!memcmp(copy,data,128)?0:81);}
    C(ready(p[0],1,1)==1);waited(pid);
    C(write(p[0],data+1024,128)==128);C(read(p[1],copy,1024)==1024);
    C(!memcmp(copy,data+128,896)&&!memcmp(copy+896,data+1024,128));
    C(send(p[0],"tail",4,0)==4&&shutdown(p[0],SHUT_WR)==0);
    C(ready(p[1],0,0)==1&&recv(p[1],copy,10,0)==4&&!memcmp(copy,"tail",4));
    C(ready(p[1],0,0)==1&&read(p[1],copy,1)==0);
    C(write(p[1],"reply",5)==5&&read(p[0],copy,5)==5);
    close(p[0]);C(write(p[1],"x",1)==-1&&errno==EPIPE);close(p[1]);
    C(socketpair(AF_UNIX,SOCK_STREAM,0,p)==0);alias=dup(p[0]);C(alias>=0);close(p[0]);
    C(write(alias,"d",1)==1&&read(p[1],copy,1)==1);close(alias);C(ready(p[1],0,1)==1&&read(p[1],copy,1)==0);close(p[1]);
    C(socketpair(AF_UNIX,SOCK_STREAM,0,p)==0);C(shutdown(p[1],SHUT_RD)==0);
    C(write(p[0],"x",1)==-1&&errno==EPIPE);close(p[0]);close(p[1]);
    puts("localcheck: socketpair, C16 payloads, backpressure, wrap, dup, shutdown and EOF passed");
}
static void names(void){
    int server,client,accepted,old,newserver,pid,fd,i;struct stat st;struct sockaddr_un a;socklen_t len;
    server=listener("/tmp/local.sock",2);C(stat("/tmp/local.sock",&st)==0&&S_ISSOCK(st.st_mode));
    client=sock();C(bind(client,(void *)&address,sizeof(address))==-1&&errno==EADDRINUSE);
    C(ready(server,0,0)==0&&connect_to(client)==0&&ready(server,0,0)==1);
    len=sizeof(a);C(accept(server,(void *)0x180000UL,&len)==-1&&errno==EFAULT&&ready(server,0,0)==1);
    len=sizeof(a);accepted=accept(server,(void *)&a,&len);C(accepted>=0&&len==1&&a.sun_family==AF_UNIX);
    len=sizeof(a);C(getsockname(accepted,(void *)&a,&len)==0&&!strcmp(a.sun_path,"/tmp/local.sock"));
    len=1;C(getpeername(client,(void *)&a,&len)==0&&len==strlen("/tmp/local.sock")+2);
    C(write(client,"old",3)==3);
    C(unlink("/tmp/local.sock")==0);fd=sock();C(connect_to(fd)==-1&&errno==ENOENT);close(fd);
    newserver=listener("/tmp/local.sock",1);fd=sock();C(connect_to(fd)==0);old=accept(newserver,0,0);C(old>=0);
    C(read(accepted,copy,3)==3&&!memcmp(copy,"old",3));
    C(write(old,"new",3)==3&&read(fd,copy,3)==3&&!memcmp(copy,"new",3));
    close(old);close(fd);close(newserver);close(client);close(accepted);close(server);
    client=sock();C(connect_to(client)==-1&&errno==ECONNREFUSED);close(client);C(unlink("/tmp/local.sock")==0);
    /* Path lookup uses inode identity, including aliases and permissions. */
    server=listener("/tmp/local.sock",1);C(link("/tmp/local.sock","/tmp/local.alias")==0);
    path("/tmp/../tmp/local.alias");client=sock();C(connect_to(client)==0);accepted=accept(server,0,0);C(accepted>=0);
    close(client);close(accepted);C(chmod("/tmp/local.sock",0600)==0);
    pid=fork();C(pid>=0);if(!pid){C(setuid(1000)==0);fd=sock();_exit(connect_to(fd)==-1&&errno==EACCES?0:82);}waited(pid);
    close(server);unlink("/tmp/local.alias");unlink("/tmp/local.sock");
    /* Closing listeners disposes of pending endpoints and wakes clients. */
    for(i=0;i<40;i++){
        server=listener("/tmp/local.sock",1);client=sock();C(connect_to(client)==0);close(server);
        C(ready(client,0,1)==1&&read(client,copy,1)==-1&&errno==ECONNRESET);close(client);
    }
    unlink("/tmp/local.sock");
    puts("localcheck: pathname bind/accept, permissions, aliases, unlink/rebind and listener cleanup passed");
}
static void waits(void){
    int server,c1,c2,pid,fd,p[2],i,full[32],count;fd_set set;struct timeval t={5,0};
    server=listener("/tmp/local.sock",1);c1=sock();c2=sock();C(connect_to(c1)==0);
    C(fcntl(c2,F_SETFL,O_NONBLOCK)==0&&connect_to(c2)==-1&&errno==EAGAIN);
    C(fcntl(c2,F_SETFL,0)==0);pid=fork();C(pid>=0);
    if(!pid){close(c1);close(c2);delay();fd=accept(server,0,0);C(fd>=0);close(fd);_exit(0);}
    C(connect_to(c2)==0);waited(pid);fd=accept(server,0,0);C(fd>=0);close(fd);close(c1);close(c2);close(server);unlink("/tmp/local.sock");
    server=listener("/tmp/local.sock",1);
    c1=sock();C(connect_to(c1)==0);count=0;
    while((fd=dup(server))>=0)full[count++]=fd;
    C(errno==EMFILE&&accept(server,0,0)==-1&&errno==EMFILE&&ready(server,0,0)==1);
    close(full[--count]);fd=accept(server,0,0);C(fd>=0);close(fd);
    while(count)close(full[--count]);close(c1);close(server);unlink("/tmp/local.sock");
    signal(SIGUSR1,signal_handler);
    C(socketpair(AF_UNIX,SOCK_STREAM,0,p)==0);pid=fork();C(pid>=0);
    if(!pid){delay();kill(getppid(),SIGUSR1);_exit(0);}
    FD_ZERO(&set);FD_SET(p[0],&set);C(select(p[0]+1,&set,0,0,&t)==-1&&errno==EINTR&&caught==1);waited(pid);
    pid=fork();C(pid>=0);if(!pid){close(p[0]);delay();C(write(p[1],"wake",4)==4);close(p[1]);_exit(0);}
    close(p[1]);C(ready(p[0],0,1)==1&&read(p[0],copy,4)==4&&!memcmp(copy,"wake",4));waited(pid);C(read(p[0],copy,1)==0);close(p[0]);
    puts("localcheck: backlog blocking/nonblocking, accept EMFILE, EINTR, fork wakeups and reuse passed");
}
static void limits(void){
    int servers[4],fd,i,j,failed=0,p[2],fill[32],count=0;char names[4][32];
    for(i=0;i<4;i++) {
        sprintf(names[i],"/tmp/local-limit-%d",i);servers[i]=listener(names[i],8);
        for(j=0;j<8;j++) {
            fd=socket(AF_UNIX,SOCK_STREAM,0);
            if(fd<0){C(errno==ENOBUFS);failed=1;break;}
            if(connect_to(fd)<0){C(errno==ENOBUFS);failed=1;close(fd);break;}
            close(fd); /* A pending server endpoint remains until accept/close. */
        }
        if(failed){i++;break;}
    }
    C(failed);
    while(i){--i;close(servers[i]);unlink(names[i]);}
    C(socketpair(AF_UNIX,SOCK_STREAM,0,p)==0);
    while((fd=dup(p[0]))>=0)fill[count++]=fd;
    C(errno==EMFILE);close(fill[--count]);
    C(socketpair(AF_UNIX,SOCK_STREAM,0,servers)==-1&&errno==EMFILE);
    /* Failed pair allocation must return its one FD and both endpoint slots. */
    fd=dup(p[0]);C(fd>=0);close(fd);
    while(count)close(fill[--count]);close(p[0]);close(p[1]);
    for(i=0;i<40;i++){C(socketpair(AF_UNIX,SOCK_STREAM,0,p)==0);close(p[0]);close(p[1]);}
    puts("localcheck: bounded endpoint exhaustion and partial socketpair rollback passed");
}
static void boundaries(void){
    char *base=malloc(70000UL),*edge;int p[2],server,client,accepted;struct sockaddr_un a;
    C(base!=NULL);edge=base+64000UL-((unsigned long)base&65535UL)-1;
    C(socketpair(AF_UNIX,SOCK_STREAM,0,(int *)edge)==0);memcpy(p,edge,sizeof(p));
    C(write(p[0],data,1024)==1024&&read(p[1],edge,1024)==1024&&!memcmp(data,edge,1024));close(p[0]);close(p[1]);
    path("/tmp/local-edge");unlink(address.sun_path);memcpy(edge,&address,sizeof(address));
    server=sock();C(bind(server,(void *)edge,sizeof(address))==0&&listen(server,1)==0);
    client=sock();C(connect(client,(void *)edge,sizeof(address))==0);accepted=accept(server,0,0);C(accepted>=0);
    close(client);close(accepted);close(server);unlink(address.sun_path);
    memset(&a,'x',sizeof(a));a.sun_family=AF_UNIX;server=sock();
    C(bind(server,(void *)&a,sizeof(a))==-1&&errno==ENAMETOOLONG);
    a.sun_path[0]=0;C(bind(server,(void *)&a,sizeof(a))==-1&&errno==EOPNOTSUPP);
    C(bind(server,(void *)0x180000UL,sizeof(a))==-1&&errno==EFAULT);close(server);free(base);
    puts("localcheck: bank-spanning addresses, payloads, FD pairs and invalid paths passed");
}
static void listener_reuse(void){
    int server=listener("/tmp/local-reuse",1),c1=sock(),c2=sock(),p[2],pid;char byte;
    C(connect_to(c1)==0&&pipe(p)==0);pid=fork();C(pid>=0);
    if(!pid){
        close(server);close(c1);close(p[0]);C(write(p[1],"r",1)==1);
        _exit(connect_to(c2)==-1&&errno==ECONNREFUSED?0:87);
    }
    close(p[1]);C(read(p[0],&byte,1)==1);delay();close(server);
    server=listener("/tmp/local-replacement",1);waited(pid);
    close(server);close(c1);close(c2);close(p[0]);unlink("/tmp/local-reuse");unlink("/tmp/local-replacement");
    puts("localcheck: blocked connect wakes on listener close without following reused slots");
}
int main(int argc,char **argv){
    int fd;
    signal(SIGPIPE,SIG_IGN);
    if(argc>1&&!strcmp(argv[1],"stale")) {
        path("/tmp/local-persist");fd=sock();C(connect_to(fd)==-1&&errno==ECONNREFUSED);
        C(bind(fd,(void *)&address,sizeof(address))==-1&&errno==EADDRINUSE);close(fd);
        C(unlink(address.sun_path)==0);fd=listener("/tmp/local-persist",1);close(fd);
        puts("LOCAL_STALE_OK");return 0;
    }
    pairs();names();waits();limits();boundaries();listener_reuse();
    fd=listener("/tmp/local-persist",1);close(fd); /* Intentionally survive reboot. */
    puts("LOCAL_OK");return 0;
}
