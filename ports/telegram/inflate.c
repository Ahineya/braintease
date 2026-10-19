/* SPDX-License-Identifier: MIT */
#include "core.h"
#include "vendor/tinf/tinflate.c"
static TgU32 crc(const TgByte *p,unsigned n) {
    TgU32 c=0xffffffffUL;unsigned i;
    while(n--){c^=*p++;for(i=0;i<8;i++)c=(c>>1)^(0xedb88320UL&(0UL-(c&1)));}
    return c^0xffffffffUL;
}
int tg_gunzip(const TgByte *p,unsigned n,TgByte *out,unsigned capacity) {
    unsigned at=10,flags,length,i;TgU32 size=capacity;
    if(n<18||p[0]!=31||p[1]!=139||p[2]!=8||(p[3]&0xe0))return -1;
    flags=p[3];
    if(flags&4){if(at+2>n-8)return -1;length=p[at]|p[at+1]<<8;at+=2;if(length>n-8-at)return -1;at+=length;}
    for(i=8;i<=16;i*=2)if(flags&i){while(at<n-8&&p[at])at++;if(at==n-8)return -1;at++;}
    if(flags&2){if(at+2>n-8||(crc(p,at)&65535UL)!=(TgU32)(p[at]|p[at+1]<<8))return -1;at+=2;}
    if(at>=n-8||tinf_uncompress(out,&size,p+at,n-8-at)!=TINF_OK||size>capacity||size!=tg_le32(p+n-4)||crc(out,size)!=tg_le32(p+n-8))return -1;
    return size;
}
