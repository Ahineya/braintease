/* SPDX-License-Identifier: MIT */
#include "internal.h"
static int same_nonce(TgTl *t,TgHandshake *h) {
    const TgByte *p=tg_raw(t,16),*q=tg_raw(t,16);
    return t->ok&&tg_equal(p,h->nonce,16)&&tg_equal(q,h->server_nonce,16);
}
void tg_handshake_free(TgClient *c) {
    if(c->handshake){tg_erase(c->handshake,sizeof(*c->handshake));free(c->handshake);c->handshake=NULL;}
}
int tg_handshake_start(TgClient *c) {
    TgTl t;TgHandshake *h;tg_handshake_free(c);c->handshake=calloc(1,sizeof(*h));h=c->handshake;
    if(!h||!tg_random(h->nonce,16)||!tg_random(h->new_nonce,32))return 0;
    tg_tl_init(&t,c->scratch,TG_MAX);tg_w32(&t,0xbe7e8ef1UL);tg_wraw(&t,h->nonce,16);c->phase=1;
    tg_state(c,TG_CONNECTING,"Authenticating Telegram server");return t.ok&&tg_send_plain(c,c->scratch,t.at);
}
static unsigned minimal(TgByte *p,unsigned n) {unsigned skip=0;while(skip+1<n&&!p[skip])skip++;if(skip)memmove(p,p+skip,n-skip);return n-skip;}
static int pq(TgClient *c,TgTl *in) {
    TgHandshake *h=c->handshake;TgTl t;const TgByte *p,*fingerprint;unsigned n,i,count,found=0,attempt;
    TgByte factors[8],rsa[256],data[192],key[32],hash[32],iv[32],exponent[3]={1,0,1};TgHash sh;
    p=tg_raw(in,16);if(!p||!tg_equal(p,h->nonce,16))return 0;p=tg_raw(in,16);if(!p)return 0;memcpy(h->server_nonce,p,16);
    p=tg_bytes(in,&n);if(!p||!n||n>8)return 0;memcpy(h->pq,p,n);h->npq=n;
    if(tg_u32(in)!=0x1cb5c415UL)return 0;count=tg_u32(in);if(count>64)return 0;
    for(i=0;i<count;i++){fingerprint=tg_raw(in,8);if(!fingerprint)return 0;if(tg_equal(fingerprint,tg_rsa_fingerprint[c->test?0:1],8))found=1;}
    if(!found||!in->ok||in->at!=in->n)return 0;
    if(rfl_crypto_call(12,0,h->pq,h->npq,NULL,0,NULL,0,factors,8)!=8)return 0;
    memcpy(h->p,factors,4);memcpy(h->q,factors+4,4);h->np=minimal(h->p,4);h->nq=minimal(h->q,4);
    tg_tl_init(&t,data,144);tg_w32(&t,0xa9f55f95UL);tg_wbytes(&t,h->pq,h->npq);tg_wbytes(&t,h->p,h->np);tg_wbytes(&t,h->q,h->nq);
    tg_wraw(&t,h->nonce,16);tg_wraw(&t,h->server_nonce,16);tg_wraw(&t,h->new_nonce,32);tg_w32(&t,c->dc+(c->test?10000:0));
    if(!t.ok||!tg_random(data+t.at,192-t.at))return 0;
    memset(iv,0,32);
    for(attempt=0;attempt<32;attempt++) {
        if(!tg_random(key,32))goto fail;
        for(i=0;i<192;i++)rsa[32+i]=data[191-i];
        tg_hash_init(&sh,0);tg_hash_add(&sh,key,32);tg_hash_add(&sh,data,192);tg_hash_end(&sh,rsa+224);
        if(!tg_ige(rsa+32,224,key,iv,0))goto fail;
        tg_hash(0,rsa+32,224,hash);for(i=0;i<32;i++)rsa[i]=key[i]^hash[i];
        for(i=0;i<256;i++)if(rsa[i]!=tg_rsa_n[c->test?0:1][i])break;
        if(i<256&&rsa[i]<tg_rsa_n[c->test?0:1][i])break;
    }
    if(attempt==32||rfl_crypto_call(4,0,rsa,256,tg_rsa_n[c->test?0:1],256,exponent,3,c->scratch,256)!=256)goto fail;
    memcpy(rsa,c->scratch,256);tg_tl_init(&t,c->scratch,TG_MAX);tg_w32(&t,0xd712e4beUL);
    tg_wraw(&t,h->nonce,16);tg_wraw(&t,h->server_nonce,16);tg_wbytes(&t,h->p,h->np);tg_wbytes(&t,h->q,h->nq);
    tg_wraw(&t,tg_rsa_fingerprint[c->test?0:1],8);tg_wbytes(&t,rsa,256);
    tg_erase(data,sizeof(data));tg_erase(key,32);tg_erase(rsa,256);tg_erase(hash,32);c->phase=2;
    return t.ok&&tg_send_plain(c,c->scratch,t.at);
fail:tg_erase(data,sizeof(data));tg_erase(key,32);tg_erase(rsa,256);tg_erase(hash,32);return 0;
}
static int client_dh(TgClient *c) {
    TgHandshake *h=c->handshake;TgTl t;TgByte gb[256],hash[20],data[384],generator[1];unsigned pad,attempt;int ok=0;
    generator[0]=h->g;
    for(attempt=0;attempt<8;attempt++){
        if(!tg_random(h->exponent,256)||!tg_modexp(generator,1,tg_prime,h->exponent,256,gb))goto done;
        if(tg_good_public(gb,tg_prime))break;
    }
    if(attempt==8||!tg_modexp(h->public_a,256,tg_prime,h->exponent,256,h->key))goto done;
    tg_erase(h->exponent,256);tg_hash(1,h->key,256,h->aux);
    tg_tl_init(&t,data+20,sizeof(data)-20);tg_w32(&t,0x6643b654UL);tg_wraw(&t,h->nonce,16);tg_wraw(&t,h->server_nonce,16);tg_wraw(&t,h->retry,8);tg_wbytes(&t,gb,256);
    if(!t.ok)goto done;tg_hash(1,data+20,t.at,hash);memcpy(data,hash,20);pad=(16-((20+t.at)&15))&15;
    if(!tg_random(data+20+t.at,pad)||!tg_ige(data,20+t.at+pad,h->tmp_key,h->tmp_iv,0))goto done;
    pad+=20+t.at;tg_tl_init(&t,c->scratch,TG_MAX);tg_w32(&t,0xf5045f1fUL);tg_wraw(&t,h->nonce,16);tg_wraw(&t,h->server_nonce,16);tg_wbytes(&t,data,pad);
    c->phase=3;ok=t.ok&&tg_send_plain(c,c->scratch,t.at);
done:tg_erase(gb,256);tg_erase(data,sizeof(data));tg_erase(hash,20);return ok;
}
static int dh(TgClient *c,TgTl *in) {
    TgHandshake *h=c->handshake;TgByte data[64],a[20],b[20],d[20],*encrypted;const TgByte *p;unsigned n,size,used;TgU32 when;TgTl t;
    if(!same_nonce(in,h))return 0;encrypted=(TgByte *)tg_bytes(in,&n);
    if(!encrypted||!n||n>1024||n%16||in->at!=in->n)return 0;
    memcpy(data,h->new_nonce,32);memcpy(data+32,h->server_nonce,16);tg_hash(1,data,48,a);
    memcpy(data,h->server_nonce,16);memcpy(data+16,h->new_nonce,32);tg_hash(1,data,48,b);
    memcpy(data,h->new_nonce,32);memcpy(data+32,h->new_nonce,32);tg_hash(1,data,64,d);
    memcpy(h->tmp_key,a,20);memcpy(h->tmp_key+20,b,12);memcpy(h->tmp_iv,b+12,8);memcpy(h->tmp_iv+8,d,20);memcpy(h->tmp_iv+28,h->new_nonce,4);
    tg_erase(data,64);tg_erase(a,20);tg_erase(b,20);tg_erase(d,20);
    if(!tg_ige(encrypted,n,h->tmp_key,h->tmp_iv,1)||n<20)return 0;
    tg_tl_init(&t,encrypted+20,n-20);if(tg_u32(&t)!=0xb5890dbaUL||!same_nonce(&t,h))return 0;
    h->g=tg_u32(&t);p=tg_bytes(&t,&size);if(!p||size!=256||!tg_group(p,h->g))return 0;
    p=tg_bytes(&t,&size);if(!p||!size||size>256)return 0;memset(h->public_a,0,256);memcpy(h->public_a+256-size,p,size);
    when=tg_u32(&t);used=t.at;if(!t.ok||n-20-used>15||!tg_good_public(h->public_a,tg_prime))return 0;
    tg_hash(1,encrypted+20,used,a);if(!tg_equal(a,encrypted,20))return 0;
    c->time_offset=(int32_t)when-(int32_t)time(NULL);return client_dh(c);
}
int tg_handshake_receive(TgClient *c,TgByte *data,unsigned n) {
    TgTl t;TgU32 constructor;TgByte expected[20],hash_input[41];const TgByte *p;unsigned i,type;TgHandshake *h=c->handshake;
    if(!h)return 0;tg_tl_init(&t,data,n);constructor=tg_u32(&t);
    if(c->phase==1&&constructor==0x05162463UL)return pq(c,&t);
    if(c->phase==2&&constructor==0xd0e8075cUL)return dh(c,&t);
    if(c->phase!=3||!same_nonce(&t,h))return 0;
    type=constructor==0x3bcbf734UL?1:constructor==0x46dc1fb9UL?2:constructor==0xa69dae02UL?3:0;
    if(!type)return 0;p=tg_raw(&t,16);if(!p||t.at!=t.n)return 0;
    memcpy(hash_input,h->new_nonce,32);hash_input[32]=type;memcpy(hash_input+33,h->aux,8);tg_hash(1,hash_input,41,expected);
    tg_erase(hash_input,sizeof(hash_input));if(!tg_equal(expected+4,p,16))return 0;
    if(type==2&&h->retries++<2){memcpy(h->retry,h->aux,8);return client_dh(c);}if(type!=1)return 0;
    memcpy(c->auth,h->key,256);for(i=0;i<8;i++)c->salt[i]=h->new_nonce[i]^h->server_nonce[i];
    c->has_key=1;c->phase=0;tg_handshake_free(c);
    if(!tg_random(c->session,8))return 0;
    return tg_rpc(c,c->import_n?TG_REQ_IMPORT:TG_REQ_EXPORT);
}
