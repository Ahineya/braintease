/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <stdio.h>
#include <string.h>
#include <netdb.h>
#include <resolv.h>
#include <arpa/inet.h>
int main(int argc,char **argv) {
    struct hostent *h;uint32_t ip;char address[16];unsigned i;const char *name;
    if(argc!=2||!strcmp(argv[1],"--help")) {
        fputs("usage: host NAME|ADDRESS\n       nslookup NAME|ADDRESS\n",stderr);
        return argc==2&&!strcmp(argv[1],"--help")?0:1;
    }
    name=argv[1];
    if(inet_pton(AF_INET,name,&ip)==1)h=gethostbyaddr(&ip,sizeof(ip),AF_INET);
    else h=gethostbyname(name);
    if(!h){herror(name);return 1;}
    for(i=0;h->h_addr_list[i];i++) {
        memcpy(&ip,h->h_addr_list[i],sizeof(ip));inet_ntop(AF_INET,&ip,address,sizeof(address));
        printf("%s has address %s\n",h->h_name,address);
    }
    return 0;
}
