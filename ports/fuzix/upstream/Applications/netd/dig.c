/* A Cheesey Dig Client, from FUZIX Applications/netd.
 * Ripple adaptation: bounded octet parsing, configurable types and TCP fallback.
 * The original struct overlays assumed an 8-bit C byte. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <resolv.h>
#include <arpa/inet.h>
static unsigned char packet[RIPPLE_DNS_PACKET];
static const char *types[]={"A","NS","CNAME","SOA","PTR","MX","TXT","AAAA","ANY"};
static unsigned codes[]={1,2,5,6,12,15,16,28,255};
static const char *typename(unsigned n) {
    unsigned i;static char b[20];
    for(i=0;i<9;i++)if(codes[i]==n)return types[i];
    sprintf(b,"TYPE%u",n);return b;
}
static unsigned typecode(const char *s) {
    unsigned i;char *end;unsigned long n;
    for(i=0;i<9;i++)if(!strcasecmp(s,types[i]))return codes[i];
    if(!strncmp(s,"TYPE",4)){n=strtoul(s+4,&end,10);if(*s&&!*end&&n&&n<=65535UL)return n;}
    return 0;
}
static void dotname(const char *s){printf("%s%s",s,!strcmp(s,".")?"":".");}
static int record(unsigned length,unsigned *offset,int brief) {
    struct ripple_dns_rr rr;unsigned p,end,i,c;char name[254],ip[16];uint32_t v;
    if(ripple_dns_rr(packet,length,offset,&rr))return -1;
    p=rr.data;end=p+rr.size;
    if(!brief){dotname(rr.name);printf("\t%lu\t%s\t%s\t",rr.ttl,rr.klass==1?"IN":"OTHER",typename(rr.type));}
    if(rr.klass==1&&rr.type==1) {
        v=(uint32_t)packet[p]|((uint32_t)packet[p+1]<<8)|((uint32_t)packet[p+2]<<16)|((uint32_t)packet[p+3]<<24);
        inet_ntop(AF_INET,&v,ip,sizeof(ip));fputs(ip,stdout);
    } else if(rr.klass==1&&rr.type==28) {
        for(i=0;i<8;i++)printf(i?":%x":"%x",ripple_dns_u16(packet+p+i*2));
    } else if(rr.klass==1&&(rr.type==2||rr.type==5||rr.type==12||rr.type==15||rr.type==6)) {
        if(rr.type==15){printf("%u ",ripple_dns_u16(packet+p));p+=2;}
        if(ripple_dns_name(packet,length,&p,name))return -1;dotname(name);
        if(rr.type==6) {
            putchar(' ');if(ripple_dns_name(packet,length,&p,name))return -1;dotname(name);
            for(i=0;i<5;i++)printf(" %lu",ripple_dns_u32(packet+p+i*4));
        }
    } else if(rr.klass==1&&rr.type==16) {
        while(p<end) {
            c=packet[p++];putchar('"');
            for(i=0;i<c;i++) {
                unsigned ch=packet[p++];
                if(ch=='"'||ch=='\\')putchar('\\');
                if(ch>=32&&ch<127)putchar(ch);else printf("\\%03u",ch);
            }
            putchar('"');if(p<end)putchar(' ');
        }
    } else {
        printf("\\# %u ",rr.size);while(p<end)printf("%02x",packet[p++]);
    }
    putchar('\n');return 0;
}
int main(int argc,char **argv) {
    const char *server=0,*name=0;unsigned type=1,i,at,count,section;int brief=0,n;
    char reversed[80],actual[16],question[254];uint32_t ip;
    for(i=1;i<(unsigned)argc;i++) {
        if(!strcmp(argv[i],"--help"))goto usage;
        if(argv[i][0]=='@'){server=argv[i]+1;continue;}
        if(!strcmp(argv[i],"+short")){brief=1;continue;}
        if(!strcmp(argv[i],"-x")) {
            if(++i>=(unsigned)argc||inet_pton(AF_INET,argv[i],&ip)!=1)goto usage;
            sprintf(reversed,"%u.%u.%u.%u.in-addr.arpa",(unsigned)(ip>>24),(unsigned)((ip>>16)&255),(unsigned)((ip>>8)&255),(unsigned)(ip&255));
            name=reversed;type=12;continue;
        }
        if(!strcmp(argv[i],"-t")){if(++i>=(unsigned)argc||!(type=typecode(argv[i])))goto usage;continue;}
        if(argv[i][0]=='-')goto usage;
        if(!name)name=argv[i];else if(!(type=typecode(argv[i])))goto usage;
    }
    if(!name){name=".";type=2;}
    n=ripple_dns_query(server,name,type,packet,sizeof(packet));
    if(n<0){herror("dig");return 1;}
    if(!brief) {
        if(server)strncpy(actual,server,15);else ripple_dns_server(actual);actual[15]=0;
        i=packet[3]&15;
        printf(";; SERVER: %s#53\n;; status: %s, id: %u\n",actual,i==0?"NOERROR":i==3?"NXDOMAIN":i==2?"SERVFAIL":i==5?"REFUSED":"ERROR",ripple_dns_u16(packet));
        printf(";; QUERY: %u, ANSWER: %u, AUTHORITY: %u, ADDITIONAL: %u\n",ripple_dns_u16(packet+4),ripple_dns_u16(packet+6),ripple_dns_u16(packet+8),ripple_dns_u16(packet+10));
    }
    at=12;if(ripple_dns_name(packet,n,&at,question))goto malformed;at+=4;
    for(section=0;section<3;section++) {
        count=ripple_dns_u16(packet+6+section*2);
        if(count&&!brief)printf("\n;; %s SECTION:\n",section==0?"ANSWER":section==1?"AUTHORITY":"ADDITIONAL");
        for(i=0;i<count;i++) {
            if(brief&&section){struct ripple_dns_rr rr;if(ripple_dns_rr(packet,n,&at,&rr))goto malformed;}
            else if(record(n,&at,brief))goto malformed;
        }
    }
    return 0;
malformed:fputs("dig: malformed DNS response\n",stderr);return 1;
usage:fputs("usage: dig [@SERVER] [NAME [A|AAAA|MX|NS|CNAME|TXT|SOA|PTR|ANY]] [+short]\n       dig [@SERVER] -x ADDRESS\n",stderr);return argc==2&&!strcmp(argv[1],"--help")?0:1;
}
