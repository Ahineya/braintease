/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <net/if.h>
#include <arpa/inet.h>
int main(int argc,char **argv) {
    struct ifreq request;struct sockaddr_in *a;
    int fd,i,op;char address[16],mask[16],gateway[16];
    if(argc>1&&!strcmp(argv[1],"--help"))goto usage;
    if(argc>1&&strcmp(argv[1],"eth0"))goto usage;
    fd=socket(AF_INET,SOCK_DGRAM,0);if(fd<0){perror("ifconfig");return 1;}
    memset(&request,0,sizeof(request));strcpy(request.ifr_name,"eth0");a=(void *)&request.ifr_addr;
    for(i=2;i<argc;i++) {
        if(!strcmp(argv[i],"up")||!strcmp(argv[i],"down")) {
            request.ifr_flags=!strcmp(argv[i],"up")?IFF_UP:0;
            if(ioctl(fd,SIOCSIFFLAGS,&request)<0)goto failed;
        } else {
            op=SIOCSIFADDR;
            if(!strcmp(argv[i],"netmask")||!strcmp(argv[i],"gateway")) {
                op=!strcmp(argv[i],"netmask")?SIOCSIFNETMASK:SIOCSIFGWADDR;
                if(++i==argc){close(fd);goto usage;}
            }
            memset(a,0,sizeof(*a));a->sin_family=AF_INET;
            if(inet_pton(AF_INET,argv[i],&a->sin_addr)!=1){fputs("ifconfig: invalid IPv4 address\n",stderr);close(fd);return 1;}
            if(ioctl(fd,op,&request)<0)goto failed;
        }
    }
    if(ioctl(fd,SIOCGIFADDR,&request)<0)goto failed;
    inet_ntop(AF_INET,&a->sin_addr,address,16);
    if(ioctl(fd,SIOCGIFNETMASK,&request)<0)goto failed;
    inet_ntop(AF_INET,&a->sin_addr,mask,16);
    if(ioctl(fd,SIOCGIFGWADDR,&request)<0)goto failed;
    inet_ntop(AF_INET,&a->sin_addr,gateway,16);
    if(ioctl(fd,SIOCGIFFLAGS,&request)<0)goto failed;
    printf("eth0  %s  link %s  mtu 1500\n",request.ifr_flags&IFF_UP?"UP":"DOWN",request.ifr_flags&IFF_LINKUP?"up":"down");
    printf("      inet %s  netmask %s  gateway %s\n",address,mask,gateway);
    close(fd);return 0;
failed:perror("ifconfig");close(fd);return 1;
usage:fputs("usage: ifconfig [eth0 [ADDRESS] [netmask MASK] [gateway ADDRESS] [up|down]]\n",stderr);return argc>1&&!strcmp(argv[1],"--help")?0:1;
}
