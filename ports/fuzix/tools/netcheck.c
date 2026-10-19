/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Test-only userspace client. Protocol replies come from an independent peer. */
/* Include sockets first: this must still select Ripple's wide ABI types. */
#include <sys/socket.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/wait.h>
static unsigned char data[900],reply[900];
static int bad(int n){printf("netcheck FAILED %d errno %d\n",n,errno);return n;}
int main(void) {
    struct sockaddr_in local,peer,source;
    int sockets[40],count=0,fd,other,n,i,status,child;
    unsigned sent=0,received=0;
    socklen_t length;
    unsigned long value=0x11223344UL;
    if(sizeof(size_t)!=2||sizeof(ssize_t)!=2||ntohl(htonl(value))!=value||ntohs(htons(0x1234))!=0x1234)return bad(1);
    if(inet_pton(AF_INET,"192.168.64.1",&peer.sin_addr)!=1||peer.sin_addr.s_addr!=0x0140a8c0UL)return bad(2);
    if(inet_pton(AF_INET,"256.0.0.1",&peer.sin_addr)!=0||inet_pton(AF_INET,"1.2.3.4.5",&peer.sin_addr)!=0)return bad(3);
    errno=0;if(socket(99,SOCK_STREAM,0)!=-1||errno!=EAFNOSUPPORT)return bad(4);
    errno=0;if(socket(AF_INET,SOCK_STREAM,IPPROTO_UDP)!=-1||errno!=EPROTONOSUPPORT)return bad(5);
    while(count<40&&(fd=socket(AF_INET,SOCK_DGRAM,0))>=0)sockets[count++]=fd;
    /* Both kernels have 32 FDs and a larger system-wide socket pool. */
    if(count!=29||errno!=EMFILE)return bad(6);
    for(i=0;i<count;i++)close(sockets[i]);
    fd=socket(AF_INET,SOCK_DGRAM,0);other=socket(AF_INET,SOCK_DGRAM,0);
    if(fd<0||other<0)return bad(7);
    memset(&local,0,sizeof(local));local.sin_family=AF_INET;local.sin_port=htons(54321);
    if(bind(fd,(void *)&local,sizeof(local)))return bad(8);
    errno=0;if(bind(other,(void *)&local,sizeof(local))!=-1||errno!=EADDRINUSE)return bad(9);
    close(other);
    length=sizeof(source);memset(&source,0,sizeof(source));
    if(getsockname(fd,(void *)&source,&length)||source.sin_port!=local.sin_port||length!=sizeof(source))return bad(10);
    if(fcntl(fd,F_SETFL,O_NONBLOCK)<0)return bad(11);
    errno=0;if(read(fd,reply,1)!=-1||errno!=EAGAIN)return bad(12);
    if(fcntl(fd,F_SETFL,0)<0)return bad(13);
    memset(&peer,0,sizeof(peer));peer.sin_family=AF_INET;peer.sin_port=htons(7000);
    inet_pton(AF_INET,"192.168.64.1",&peer.sin_addr);
    for(i=0;i<900;i++)data[i]=i&255;
    if(sendto(fd,data,900,0,(void *)&peer,sizeof(peer))!=900)return bad(14);
    length=sizeof(source);n=recvfrom(fd,reply,900,0,(void *)&source,&length);
    if(n!=900||memcmp(data,reply,900)||source.sin_addr.s_addr!=peer.sin_addr.s_addr||source.sin_port!=peer.sin_port)return bad(15);
    if(sendto(fd,data,0,0,(void *)&peer,sizeof(peer))!=0)return bad(16);
    length=sizeof(source);if(recvfrom(fd,reply,900,0,(void *)&source,&length)!=0)return bad(17);
    errno=0;if(sendto(fd,data,900,1,(void *)&peer,sizeof(peer))!=-1||errno!=EINVAL)return bad(18);
    close(fd);puts("netcheck: sockets, datagrams, binary data and nonblocking I/O passed");
    fd=socket(AF_INET,SOCK_STREAM,0);peer.sin_port=htons(7001);
    if(fd<0||connect(fd,(void *)&peer,sizeof(peer)))return bad(19);
    other=dup(fd);if(other<0)return bad(20);close(fd);fd=other;
    length=sizeof(source);
    if(getpeername(fd,(void *)&source,&length)||source.sin_port!=peer.sin_port)return bad(21);
    while(sent<900){n=write(fd,data+sent,900-sent);if(n<=0)return bad(22);sent+=n;}
    if(shutdown(fd,1))return bad(23);
    while(received<900){n=read(fd,reply+received,900-received);if(n<=0)return bad(24);received+=n;}
    if(memcmp(data,reply,900)||read(fd,reply,1)!=0)return bad(25);
    child=fork();if(child<0)return bad(26);
    if(!child){close(fd);_exit(17);}
    if(waitpid(child,&status,0)!=child||!WIFEXITED(status)||WEXITSTATUS(status)!=17)return bad(27);
    if(read(fd,reply,1)!=0)return bad(28);
    close(fd);puts("netcheck: TCP, partial streams, dup/fork references and half-close passed");
    return 0;
}
