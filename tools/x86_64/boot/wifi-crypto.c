/* WPA2 SHA1 PRF/MIC sequencing follows OpenBSD ieee80211_crypto.c; BSD notices
 * in wifi-LICENSE.txt. SHA1/AES cores pinned separately. RFC3394 unwrap never
 * exposes unauthenticated plaintext, including when input/output overlap. */
#include "wifi-crypto-internal.h"
void wifi_wipe(void *p,size_t n){volatile uint8_t *v=p;while(n--)*v++=0;}
int wifi_equal(const uint8_t *a,const uint8_t *b,size_t n){unsigned d=0;for(size_t i=0;i<n;i++)d|=a[i]^b[i];return d==0;}
void wifi_sha1(const uint8_t *p,size_t n,uint8_t out[20]){SHA1_CTX s;SHA1Init(&s);SHA1Update(&s,p,n);SHA1Final(out,&s);}
void wifi_hmac_sha1(const uint8_t *key,size_t kn,const uint8_t *p,size_t n,uint8_t out[20]) {
    uint8_t pad[64]={0},inner[20];SHA1_CTX s;
    if(kn>64)wifi_sha1(key,kn,pad);else memcpy(pad,key,kn);
    for(unsigned i=0;i<64;i++)pad[i]^=0x36;
    SHA1Init(&s);SHA1Update(&s,pad,64);SHA1Update(&s,p,n);SHA1Final(inner,&s);
    for(unsigned i=0;i<64;i++)pad[i]^=0x36^0x5c;
    SHA1Init(&s);SHA1Update(&s,pad,64);SHA1Update(&s,inner,20);SHA1Final(out,&s);
    wifi_wipe(pad,sizeof(pad));wifi_wipe(inner,sizeof(inner));
}
int wifi_pmk(const uint8_t *pass,unsigned pn,const uint8_t *ssid,unsigned sn,uint8_t out[32]) {
    if(pn<8 || pn>63 || !sn || sn>32)return 0;
    for(unsigned i=0;i<pn;i++)if(pass[i]<32 || pass[i]>126)return 0;
    uint8_t salt[36],u[20],t[20];memcpy(salt,ssid,sn);
    for(unsigned block=1;block<=2;block++) {
        memset(salt+sn,0,4);salt[sn+3]=block;
        wifi_hmac_sha1(pass,pn,salt,sn+4,u);memcpy(t,u,20);
        for(unsigned j=1;j<4096;j++){wifi_hmac_sha1(pass,pn,u,20,u);for(unsigned i=0;i<20;i++)t[i]^=u[i];}
        memcpy(out+(block-1)*20,t,block==1?20:12);
    }
    wifi_wipe(salt,sizeof(salt));wifi_wipe(u,sizeof(u));wifi_wipe(t,sizeof(t));return 1;
}
static int less(const uint8_t *a,const uint8_t *b,unsigned n) {
    for(unsigned i=0;i<n;i++){if(a[i]<b[i])return 1;if(a[i]>b[i])return 0;}return 0;
}
void wifi_ptk(const uint8_t pmk[32],const uint8_t aa[6],const uint8_t sa[6],const uint8_t an[32],const uint8_t sn[32],uint8_t out[48]) {
    uint8_t buf[100],digest[20];static const uint8_t label[]="Pairwise key expansion";
    memcpy(buf,label,sizeof(label));unsigned at=sizeof(label);int order=less(aa,sa,6);
    memcpy(buf+at,order?aa:sa,6);memcpy(buf+at+6,order?sa:aa,6);at+=12;order=less(an,sn,32);
    memcpy(buf+at,order?an:sn,32);memcpy(buf+at+32,order?sn:an,32);at+=64;
    for(unsigned i=0;i<3;i++){buf[at]=i;wifi_hmac_sha1(pmk,32,buf,at+1,digest);memcpy(out+20*i,digest,i==2?8:20);}
    wifi_wipe(buf,sizeof(buf));wifi_wipe(digest,sizeof(digest));
}
int wifi_unwrap(const uint8_t kek[16],const uint8_t *in,unsigned bytes,uint8_t *out) {
    if(bytes<24 || bytes>512 || bytes%8)return 0;
    uint8_t tmp[512],block[16],a[8];unsigned n=bytes/8-1;rijndael_ctx ctx;
    if(rijndael_set_key(&ctx,kek,128))return 0;
    memcpy(a,in,8);memcpy(tmp,in+8,bytes-8);
    for(int j=5;j>=0;j--)for(unsigned i=n;i;i--) {
        uint64_t t=(uint64_t)n*j+i;
        for(unsigned k=0;k<8;k++)block[k]=a[k]^(uint8_t)(t>>(8*(7-k)));
        memcpy(block+8,tmp+(i-1)*8,8);rijndael_decrypt(&ctx,block,block);
        memcpy(a,block,8);memcpy(tmp+(i-1)*8,block+8,8);
    }
    static const uint8_t iv[8]={0xa6,0xa6,0xa6,0xa6,0xa6,0xa6,0xa6,0xa6};
    int ok=wifi_equal(a,iv,8);if(ok)memcpy(out,tmp,bytes-8);
    wifi_wipe(&ctx,sizeof(ctx));wifi_wipe(tmp,sizeof(tmp));wifi_wipe(block,sizeof(block));wifi_wipe(a,sizeof(a));return ok;
}
