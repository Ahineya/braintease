/* SPDX-License-Identifier: MIT */
/* Streaming SHA-1 (MTProto key identifiers only) and SHA-256. C16 octets are
 * explicitly masked; uint32_t is 32 bits on both host and guest builds. */
#include "core.h"
static TgU32 rotate(TgU32 x,unsigned n) {return (x>>n)|(x<<(32-n));}
static const TgU32 k[64]={
0x428a2f98UL,0x71374491UL,0xb5c0fbcfUL,0xe9b5dba5UL,0x3956c25bUL,0x59f111f1UL,0x923f82a4UL,0xab1c5ed5UL,
0xd807aa98UL,0x12835b01UL,0x243185beUL,0x550c7dc3UL,0x72be5d74UL,0x80deb1feUL,0x9bdc06a7UL,0xc19bf174UL,
0xe49b69c1UL,0xefbe4786UL,0x0fc19dc6UL,0x240ca1ccUL,0x2de92c6fUL,0x4a7484aaUL,0x5cb0a9dcUL,0x76f988daUL,
0x983e5152UL,0xa831c66dUL,0xb00327c8UL,0xbf597fc7UL,0xc6e00bf3UL,0xd5a79147UL,0x06ca6351UL,0x14292967UL,
0x27b70a85UL,0x2e1b2138UL,0x4d2c6dfcUL,0x53380d13UL,0x650a7354UL,0x766a0abbUL,0x81c2c92eUL,0x92722c85UL,
0xa2bfe8a1UL,0xa81a664bUL,0xc24b8b70UL,0xc76c51a3UL,0xd192e819UL,0xd6990624UL,0xf40e3585UL,0x106aa070UL,
0x19a4c116UL,0x1e376c08UL,0x2748774cUL,0x34b0bcb5UL,0x391c0cb3UL,0x4ed8aa4aUL,0x5b9cca4fUL,0x682e6ff3UL,
0x748f82eeUL,0x78a5636fUL,0x84c87814UL,0x8cc70208UL,0x90befffaUL,0xa4506cebUL,0xbef9a3f7UL,0xc67178f2UL};
static void block(TgHash *h) {
    TgU32 w[80],a,b,c,d,e,f,g,j,t,u;unsigned i;
    for(i=0;i<16;i++)w[i]=(TgU32)h->block[4*i]<<24|(TgU32)h->block[4*i+1]<<16|(TgU32)h->block[4*i+2]<<8|h->block[4*i+3];
    a=h->h[0];b=h->h[1];c=h->h[2];d=h->h[3];e=h->h[4];
    if(h->sha1){
        for(i=16;i<80;i++)w[i]=rotate(w[i-3]^w[i-8]^w[i-14]^w[i-16],31);
        for(i=0;i<80;i++) {
            if(i<20){f=(b&c)|(~b&d);u=0x5a827999UL;}
            else if(i<40){f=b^c^d;u=0x6ed9eba1UL;}
            else if(i<60){f=(b&c)|(b&d)|(c&d);u=0x8f1bbcdcUL;}
            else{f=b^c^d;u=0xca62c1d6UL;}
            t=rotate(a,27)+f+e+u+w[i];e=d;d=c;c=rotate(b,2);b=a;a=t;
        }
    }else{
        for(i=16;i<64;i++)w[i]=w[i-16]+(rotate(w[i-15],7)^rotate(w[i-15],18)^(w[i-15]>>3))+
            w[i-7]+(rotate(w[i-2],17)^rotate(w[i-2],19)^(w[i-2]>>10));
        f=h->h[5];g=h->h[6];j=h->h[7];
        for(i=0;i<64;i++) {
            t=j+(rotate(e,6)^rotate(e,11)^rotate(e,25))+((e&f)^(~e&g))+k[i]+w[i];
            u=(rotate(a,2)^rotate(a,13)^rotate(a,22))+((a&b)^(a&c)^(b&c));
            j=g;g=f;f=e;e=d+t;d=c;c=b;b=a;a=t+u;
        }
        h->h[5]+=f;h->h[6]+=g;h->h[7]+=j;
    }
    h->h[0]+=a;h->h[1]+=b;h->h[2]+=c;h->h[3]+=d;h->h[4]+=e;
    tg_erase(w,sizeof(w));
}
void tg_hash_init(TgHash *h,int sha1) {
    static const TgU32 initial[8]={0x6a09e667UL,0xbb67ae85UL,0x3c6ef372UL,0xa54ff53aUL,0x510e527fUL,0x9b05688cUL,0x1f83d9abUL,0x5be0cd19UL};
    memset(h,0,sizeof(*h));h->sha1=sha1;
    if(sha1){h->h[0]=0x67452301UL;h->h[1]=0xefcdab89UL;h->h[2]=0x98badcfeUL;h->h[3]=0x10325476UL;h->h[4]=0xc3d2e1f0UL;}
    else memcpy(h->h,initial,sizeof(initial));
}
void tg_hash_add(TgHash *h,const TgByte *p,unsigned n) {
    unsigned count;h->total+=n;
    while(n){count=64-h->used;if(count>n)count=n;memcpy(h->block+h->used,p,count);h->used+=count;p+=count;n-=count;
        if(h->used==64){block(h);h->used=0;}}
}
void tg_hash_end(TgHash *h,TgByte *out) {
    TgU32 bits=h->total*8UL;unsigned i,count=h->sha1?20:32;
    h->block[h->used++]=128;
    if(h->used>56){memset(h->block+h->used,0,64-h->used);block(h);h->used=0;}
    memset(h->block+h->used,0,64-h->used);
    for(i=0;i<4;i++){h->block[63-i]=bits&255;bits>>=8;}block(h);
    for(i=0;i<count;i++)out[i]=(h->h[i/4]>>(24-8*(i%4)))&255;
    tg_erase(h,sizeof(*h));
}
void tg_hash(int sha1,const TgByte *p,unsigned n,TgByte *out) {TgHash h;tg_hash_init(&h,sha1);tg_hash_add(&h,p,n);tg_hash_end(&h,out);}
