/* FUZIX Echoping - UDP test client. Ripple: hostname lookup, bounded input,
 * connected-peer filtering, monotonic timeout and meaningful exit status. */
#include <netutil.h>
#include <netdb.h>
#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>
int main(int argc,char **argv) {
    struct sockaddr_in peer;struct hostent *h;char reply[1024],*end;
    const char *message="This is a test!";unsigned long port=7,start;int fd,n,status=1;
    if(argc<2||argc>4||!strcmp(argv[1],"--help"))goto usage;
    if(argc>2){port=strtoul(argv[2],&end,10);if(!*argv[2]||*end||!port||port>65535UL)goto usage;}
    if(argc>3)message=argv[3];if(strlen(message)>1023){fputs("echoping: message too long\n",stderr);return 1;}
    h=gethostbyname(argv[1]);if(!h){herror(argv[1]);return 1;}
    memset(&peer,0,sizeof(peer));peer.sin_family=AF_INET;peer.sin_port=htons(port);
    memcpy(&peer.sin_addr,h->h_addr_list[0],sizeof(peer.sin_addr));
    fd=socket(AF_INET,SOCK_DGRAM,0);if(fd<0){perror("echoping");return 1;}
    if(connect(fd,(void *)&peer,sizeof(peer))||fcntl(fd,F_SETFL,O_NONBLOCK)<0||nu_write(fd,message,strlen(message),5000UL))goto done;
    if(!*message) {
        start=nu_now();
        /* A zero-length write is a generic filesystem no-op; send must
         * reach the socket layer to transmit an empty UDP datagram. */
        do {n=send(fd,message,0,0);if(n>=0)break;if(errno!=EAGAIN||nu_expired(start,5000UL)||nu_wait())goto done;}while(1);
    }
    n=nu_read(fd,reply,sizeof(reply)-1,5000UL);if(n<0)goto done;
    reply[n]=0;
    if((unsigned)n!=strlen(message)||memcmp(reply,message,n)){fputs("echoping: reply differs from message\n",stderr);close(fd);return 1;}
    printf("Received %d octets from %s: %s\n",n,argv[1],reply);status=0;
 done:if(status)perror("echoping");close(fd);return status;
 usage:fputs("usage: echoping HOST [PORT [MESSAGE]]\n",stderr);return argc==2&&!strcmp(argv[1],"--help")?0:1;
}
