/* SPDX-License-Identifier: MIT */
#include "packet.h"
static void aes(const TgByte *auth,const TgByte *msg,unsigned x,TgByte *key,TgByte *iv) {
    TgHash h;TgByte a[32],b[32];
    tg_hash_init(&h,0);tg_hash_add(&h,msg,16);tg_hash_add(&h,auth+x,36);tg_hash_end(&h,a);
    tg_hash_init(&h,0);tg_hash_add(&h,auth+40+x,36);tg_hash_add(&h,msg,16);tg_hash_end(&h,b);
    memcpy(key,a,8);memcpy(key+8,b+8,16);memcpy(key+24,a+24,8);
    memcpy(iv,b,8);memcpy(iv+8,a+8,16);memcpy(iv+24,b+24,8);tg_erase(a,32);tg_erase(b,32);
}
int tg_encrypt(const TgByte *auth,const TgByte *salt,const TgByte *session,TgU64 id,TgU32 seq,
    const TgByte *body,unsigned n,TgByte *out,unsigned cap,unsigned x) {
    TgByte h[32],key[32],iv[32];TgHash hash;unsigned pad=(16-((32+n+12)&15))&15,total=32+n+12+pad;int ok;
    if((x!=0&&x!=8)||n>TG_MAX-128||n%4||cap<total+24)return -1;
    memcpy(out+24,salt,8);memcpy(out+32,session,8);tg_put64(out+40,id);tg_put32(out+48,seq);tg_put32(out+52,n);memcpy(out+56,body,n);
    if(!tg_random(out+56+n,12+pad))return -1;
    tg_hash(1,auth,256,h);memcpy(out,h+12,8);
    tg_hash_init(&hash,0);tg_hash_add(&hash,auth+88+x,32);tg_hash_add(&hash,out+24,total);tg_hash_end(&hash,h);memcpy(out+8,h+8,16);
    aes(auth,out+8,x,key,iv);ok=tg_ige(out+24,total,key,iv,0);tg_erase(key,32);tg_erase(iv,32);tg_erase(h,32);return ok?(int)(total+24):-1;
}
int tg_decrypt(const TgByte *auth,TgByte *packet,unsigned n,unsigned x) {
    TgByte key[32],iv[32],h[32];TgHash hash;TgU32 length;unsigned pad;int ok;
    if((x!=0&&x!=8)||n<72||n>TG_MAX||(n-24)%16)return -1;
    tg_hash(1,auth,256,h);if(!tg_equal(h+12,packet,8))return -1;
    aes(auth,packet+8,x,key,iv);ok=tg_ige(packet+24,n-24,key,iv,1);tg_erase(key,32);tg_erase(iv,32);if(!ok)return -1;
    tg_hash_init(&hash,0);tg_hash_add(&hash,auth+88+x,32);tg_hash_add(&hash,packet+24,n-24);tg_hash_end(&hash,h);
    if(!tg_equal(h+8,packet+8,16)){tg_erase(packet+24,n-24);return -1;}
    length=tg_le32(packet+52);if(length>n-56||length%4)return -1;pad=n-56-length;
    if(pad<12||pad>1024)return -1;return (int)length;
}
