/* Address/link readiness only: no DNS/internet probe or radio policy. */
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <net/if.h>
int main(void) {
    struct ifreq r;struct sockaddr_in *a;int fd=socket(AF_INET,SOCK_DGRAM,0);
    if(fd<0){perror("net-ready");return 1;}
    memset(&r,0,sizeof(r));strcpy(r.ifr_name,"eth0");
    if(ioctl(fd,SIOCGIFFLAGS,&r)<0)goto failed;
    if((r.ifr_flags&(IFF_UP|IFF_LINKUP))!=(IFF_UP|IFF_LINKUP)) {
        fputs("net-ready: interface/link is down\n",stderr);close(fd);return 1;
    }
    if(ioctl(fd,SIOCGIFADDR,&r)<0)goto failed;
    a=(void *)&r.ifr_addr;
    if(!a->sin_addr.s_addr){fputs("net-ready: no IPv4 address\n",stderr);close(fd);return 1;}
    close(fd);puts("NETWORK_ADDRESS_READY");return 0;
failed:perror("net-ready");close(fd);return 1;
}
