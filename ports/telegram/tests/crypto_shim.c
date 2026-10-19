/* Independent OpenSSL oracle, for host tests only. Never linked into guests. */
#include "../internal.h"
#include "../model.h"
#include <openssl/aes.h>
#include <openssl/bn.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
int rfl_crypto_random(TgByte *out,unsigned long n) {return RAND_bytes(out,n)==1?0:-1;}
long rfl_crypto_call(unsigned op,unsigned parameter,const TgByte *a,unsigned long na,
    const TgByte *b,unsigned long nb,const TgByte *c,unsigned long nc,TgByte *out,unsigned long capacity) {
    BN_CTX *ctx;BIGNUM *x,*n,*e,*r;long result=-1;AES_KEY key;TgByte iv[32];unsigned long count;
    if(op==0&&parameter==1&&capacity>=3){out[0]=1;out[1]=254;out[2]=31;return 3;}
    if(parameter)return -1;
    if(op==4||op==5||op==11){
        if(capacity<nb)return -1;ctx=BN_CTX_new();x=BN_bin2bn(a,na,NULL);n=BN_bin2bn(b,nb,NULL);e=BN_bin2bn(c,nc,NULL);r=BN_new();
        if((op==11?BN_mod_mul(r,x,e,n,ctx):BN_mod_exp(r,x,e,n,ctx))==1&&BN_bn2binpad(r,out,nb)==(int)nb)result=nb;
        BN_clear_free(x);BN_clear_free(n);BN_clear_free(e);BN_clear_free(r);BN_CTX_free(ctx);return result;
    }
    if(op==6){count=((unsigned long)c[0]<<24)|((unsigned long)c[1]<<16)|((unsigned long)c[2]<<8)|c[3];
        return capacity>=64&&PKCS5_PBKDF2_HMAC((const char *)a,na,b,nb,count,EVP_sha512(),64,out)==1?64:-1;}
    if(op==7||op==8){
        if(nb!=32||nc!=32||na%16||capacity<na)return -1;memcpy(iv,c,32);
        if(op==7)AES_set_encrypt_key(b,256,&key);else AES_set_decrypt_key(b,256,&key);
        AES_ige_encrypt(a,out,na,&key,iv,op==7?AES_ENCRYPT:AES_DECRYPT);tg_erase(&key,sizeof(key));return na;
    }
    if(op==12){
        TgU64 value=0,p;unsigned i;for(i=0;i<na;i++)value=(value<<8)|a[i];
        for(p=3;p<1000000&&p*p<=value;p+=2)if(value%p==0){TgU64 q=value/p;if(capacity<8)return -1;
            for(i=0;i<4;i++){out[3-i]=(p>>(8*i))&255;out[7-i]=(q>>(8*i))&255;}return 8;}return -1;
    }
    return -1;
}
/* The client uses direct bootstrap IPs. Host tests must fail if they ever
 * accidentally enter the guest resolver instead of the tested socket path. */
void rfl_dns_close(RflWait *p){(void)p;}
void rfl_dns_destroy(RflWait *p){(void)p;}
void rfl_dns_step(RflWait *p,unsigned long n){(void)p;(void)n;abort();}
int rfl_dns_fd(RflWait *p){(void)p;return -1;}
unsigned rfl_dns_events(RflWait *p){(void)p;return 0;}
unsigned long rfl_dns_delay(RflWait *p,unsigned long n,unsigned long d){(void)p;(void)n;return d;}
TgClient *tg_test_client(const char *cfg,const char *session,unsigned port) {
    TgClient *c=tg_client_new(cfg,session);if(c){c->fixture_ip[0]=127;c->fixture_ip[1]=c->fixture_ip[2]=0;c->fixture_ip[3]=1;c->fixture_port=port;}return c;
}
const char *tg_test_debug(TgClient *c) {
    static char text[192];
    snprintf(text,sizeof(text),"state=%u request=%u dc=%u rx=%u tx=%u phase=%u pending=%u socket=%u seen=%u",c->state,c->request,c->dc,c->rx_n,c->tx_n,c->phase,c->connecting!=NULL,c->socket!=NULL,c->seen_n);
    return text;
}
int tg_test_safe_prime(const TgByte *p) {
    BN_CTX *ctx=BN_CTX_new();BIGNUM *n=BN_bin2bn(p,256,NULL),*q=BN_dup(n);int ok;
    BN_sub_word(q,1);BN_rshift1(q,q);ok=BN_check_prime(n,ctx,NULL)==1&&BN_check_prime(q,ctx,NULL)==1;
    BN_free(n);BN_free(q);BN_CTX_free(ctx);return ok;
}
TgClient *tg_test_receiver(const TgByte *auth,const TgByte *salt,const TgByte *session) {
    TgClient *c=calloc(1,sizeof(*c));if(!c)return NULL;
    memcpy(c->auth,auth,256);memcpy(c->salt,salt,8);memcpy(c->session,session,8);c->has_key=c->authorized=1;c->state=TG_READY;
    return c;
}
TgClient *tg_test_model(void){TgClient *c=calloc(1,sizeof(*c));if(!c)return NULL;c->user_id=42;c->authorized=c->has_key=1;c->state=TG_READY;
    if(!tg_client_browse(c)){free(c);return NULL;}return c;}
const char *tg_test_model_debug(TgClient *c){static char out[192];TgModel *m=c->model;snprintf(out,sizeof(out),"pts=%lu seq=%lu diff=%u stage=%u messages=%u",(unsigned long)m->pts,(unsigned long)m->seq,m->need_diff,m->stage,m->messages_n);return out;}
int tg_test_model_query(TgClient *c,unsigned kind,TgByte *out,unsigned cap){TgTl t;tg_tl_init(&t,out,cap);return tg_model_query(c,&t,kind)&&t.ok?(int)t.at:-1;}
const TgByte *tg_test_last_query(TgClient *c){return c->last_query;}
void tg_test_channel(TgClient *c,TgU64 id){TgPeerId p={TG_CHANNEL,0,0};p.id=id;c->model->channel_peer=tg_peer_slot(c,p,0);}
TgU64 tg_test_recipient(TgClient *c){return c->model->outbox_peer.id;}
const TgByte *tg_test_salt(TgClient *c){return c->salt;}
