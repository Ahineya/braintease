/* SPDX-License-Identifier: MIT */
#include "core.h"
#include <vm_crypto.h>
int tg_random(TgByte *p,unsigned n) {return rfl_crypto_random(p,n)==0;}
int tg_modexp(const TgByte *a,unsigned na,const TgByte *p,const TgByte *e,unsigned ne,TgByte *out) {
    return rfl_crypto_call(VM_CRYPTO_MODEXP_2048,0,a,na,p,256,e,ne,out,256)==256;
}
int tg_modmul(const TgByte *a,const TgByte *p,const TgByte *b,TgByte *out) {
    return rfl_crypto_call(VM_CRYPTO_MODMUL_2048,0,a,256,p,256,b,256,out,256)==256;
}
int tg_ige(TgByte *p,unsigned n,const TgByte *key,const TgByte *initial,int decrypt) {
    TgByte iv[32],next[32],out[512];unsigned at=0,take;int ok=0;
    if(n%16)return 0;memcpy(iv,initial,32);
    while(at<n){
        take=n-at;if(take>512)take=512;
        memcpy(next+(decrypt?0:16),p+at+take-16,16);
        if(rfl_crypto_call(decrypt?VM_CRYPTO_AES256_IGE_DECRYPT:VM_CRYPTO_AES256_IGE_ENCRYPT,
            0,p+at,take,key,32,iv,32,out,take)!=(long)take)goto done;
        memcpy(next+(decrypt?16:0),out+take-16,16);memcpy(p+at,out,take);memcpy(iv,next,32);at+=take;
    }
    ok=1;
done:tg_erase(iv,sizeof(iv));tg_erase(next,sizeof(next));tg_erase(out,sizeof(out));return ok;
}
static int compare(const TgByte *a,const TgByte *b,unsigned n) {
    unsigned i;for(i=0;i<n;i++)if(a[i]!=b[i])return a[i]>b[i]?1:-1;return 0;
}
int tg_group(const TgByte *p,unsigned g) {
    unsigned r=0,i;
    if(!tg_equal(p,tg_prime,256)||g<2||g>7)return 0;
    for(i=0;i<256;i++)r=(r*256+p[i])%(4*g);
    return g==2?r%8==7:g==3?r%3==2:g==4?1:g==5?(r%5==1||r%5==4):g==6?(r%24==19||r%24==23):(r%7==3||r%7==5||r%7==6);
}
static void subtract(const TgByte *a,const TgByte *b,TgByte *out) {
    int i,d;unsigned borrow=0;
    for(i=255;i>=0;i--){d=(int)a[i]-b[i]-borrow;out[i]=d&255;borrow=d<0;}
}
int tg_good_public(const TgByte *v,const TgByte *p) {
    TgByte bound[256],upper[256];memset(bound,0,256);bound[7]=1;
    subtract(p,bound,upper);return compare(v,bound,256)>0&&compare(v,upper,256)<0;
}
static void salted(const TgByte *p,unsigned n,const TgByte *salt,unsigned ns,TgByte *out) {
    TgHash h;tg_hash_init(&h,0);tg_hash_add(&h,salt,ns);tg_hash_add(&h,p,n);tg_hash_add(&h,salt,ns);tg_hash_end(&h,out);
}
/* Caller owns password and returned A/M1. Every intermediate is erased. */
int tg_srp(const TgByte *password,unsigned np,const TgByte *s1,unsigned n1,const TgByte *s2,unsigned n2,
    unsigned g,const TgByte *p,const TgByte *B,TgByte *A,TgByte *M1) {
    struct Work {TgByte a[256],x[32],v[256],gp[256],kv[256],t[256],u[32],k[32],S[256],K[32],pb[64],e[257],product[65],hp[32],hg[32];} *w;
    TgByte count[4]={0,1,134,160};TgHash h;unsigned i,j,carry,tries;int pos,d,borrow,ok=0;
    if(np>512||n1>128||n2>128||!tg_group(p,g)||!tg_good_public(B,p))return 0;
    w=calloc(1,sizeof(*w));if(!w)return 0;w->gp[255]=g;
    salted(password,np,s1,n1,w->x);salted(w->x,32,s2,n2,w->x);
    if(rfl_crypto_call(VM_CRYPTO_PBKDF2_SHA512,0,w->x,32,s1,n1,count,4,w->pb,64)!=64)goto done;
    salted(w->pb,64,s2,n2,w->x);
    if(!tg_modexp(&w->gp[255],1,p,w->x,32,w->v))goto done;
    tg_hash_init(&h,0);tg_hash_add(&h,p,256);tg_hash_add(&h,w->gp,256);tg_hash_end(&h,w->k);
    memset(w->kv,0,256);memcpy(w->kv+224,w->k,32);
    if(!tg_modmul(w->v,p,w->kv,w->kv))goto done;
    borrow=0;for(pos=255;pos>=0;pos--){d=(int)B[pos]-w->kv[pos]-borrow;w->t[pos]=d&255;borrow=d<0;}
    carry=0;for(pos=255;pos>=0;pos--){i=w->t[pos]+(p[pos]&(0U-(unsigned)borrow))+carry;w->t[pos]=i&255;carry=i>>8;}
    if(!tg_good_public(w->t,p))goto done;
    for(tries=0;tries<8;tries++){
        if(!tg_random(w->a,256)||!tg_modexp(&w->gp[255],1,p,w->a,256,A))goto done;
        if(tg_good_public(A,p))break;
    }
    if(tries==8)goto done;
    tg_hash_init(&h,0);tg_hash_add(&h,A,256);tg_hash_add(&h,B,256);tg_hash_end(&h,w->u);
    carry=0;for(i=0;i<32;i++)carry|=w->u[i];if(!carry)goto done;
    for(pos=31;pos>=0;pos--){carry=0;for(d=31;d>=0;d--){j=pos+d+2;i=w->product[j]+w->u[pos]*w->x[d]+carry;w->product[j]=i&255;carry=i>>8;}w->product[pos+1]=carry;}
    memcpy(w->e+1,w->a,256);carry=0;
    for(pos=256;pos>=0;pos--){i=w->e[pos]+(pos>=192?w->product[pos-192]:0)+carry;w->e[pos]=i&255;carry=i>>8;}
    if(!tg_modexp(w->t,256,p,w->e,257,w->S))goto done;
    tg_hash(0,w->S,256,w->K);tg_hash(0,p,256,w->hp);tg_hash(0,w->gp,256,w->hg);
    for(i=0;i<32;i++)w->hp[i]^=w->hg[i];
    tg_hash_init(&h,0);tg_hash_add(&h,w->hp,32);tg_hash(0,s1,n1,w->hp);tg_hash_add(&h,w->hp,32);
    tg_hash(0,s2,n2,w->hp);tg_hash_add(&h,w->hp,32);tg_hash_add(&h,A,256);tg_hash_add(&h,B,256);tg_hash_add(&h,w->K,32);tg_hash_end(&h,M1);ok=1;
done:tg_erase(w,sizeof(*w));free(w);if(!ok){tg_erase(A,256);tg_erase(M1,32);}return ok;
}
