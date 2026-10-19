/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef C16_BROWSER_ADDRESS_H
#define C16_BROWSER_ADDRESS_H
#include <stddef.h>
#include <string.h>
static int address_prefix(const char *s,size_t length,const char *prefix) {
    size_t i,n=strlen(prefix);unsigned c;
    if(length<n)return 0;
    for(i=0;i<n;i++){c=(unsigned char)s[i];if(c>='A'&&c<='Z')c+='a'-'A';if(c!=(unsigned)prefix[i])return 0;}
    return 1;
}
/* Never truncate a URL into a different destination. No implicit HTTP downgrade;
 * callers may still enter http:// explicitly for sites that require it. */
static int browser_address(const char *input,char *out,size_t capacity) {
    const char *end=input+strlen(input),*p,*colon=NULL,*prefix="https://";
    size_t n,extra;
    while(input<end&&(unsigned char)*input<=32)input++;
    while(end>input&&(unsigned char)end[-1]<=32)end--;
    if(input==end)return -1;
    n=end-input;
    for(p=input;p<end;p++)if((unsigned char)*p<32||(unsigned char)*p==127)return -1;
    if(*input=='/'&&n>1&&input[1]=='/')prefix="https:";
    else if(*input=='/')prefix="file://";
    else if(address_prefix(input,n,"https://")||address_prefix(input,n,"http://")||
            address_prefix(input,n,"file:")||address_prefix(input,n,"about:")||address_prefix(input,n,"data:"))prefix="";
    else {
        /* A hostname:port is not a scheme. Bracketed IPv6 is an authority too. */
        if(*input!='[') {
            for(p=input;p<end&&*p!='/'&&*p!='?'&&*p!='#';p++)if(*p==':'){colon=p;break;}
            if(colon) {
                p=colon+1;if(p==end||*p<'0'||*p>'9')return -1;
                while(p<end&&*p>='0'&&*p<='9')p++;
                if(p<end&&*p!='/'&&*p!='?'&&*p!='#')return -1;
            }
        }
    }
    extra=strlen(prefix);if(extra>=capacity||n>=capacity-extra)return -1;
    memcpy(out,prefix,extra);memcpy(out+extra,input,n);out[extra+n]=0;return 0;
}
#endif
