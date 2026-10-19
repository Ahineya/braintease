/* wolfCrypt callback adapter: only arithmetic crosses /dev/crypto. */
#include "ripple_crypto.h"
#include <wolfssl/wolfcrypt/cryptocb.h>
#include <wolfssl/wolfcrypt/error-crypt.h>
#include <vm_crypto.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
static unsigned initialized,enabled,bulk_enabled;
static unsigned long calls[15],milliseconds[15];
static unsigned long stamp(void){struct timespec t;if(clock_gettime(CLOCK_MONOTONIC,&t))return 0;return (unsigned long)t.tv_sec*1000UL+t.tv_nsec/1000000UL;}
static void clear(void *p,unsigned n){volatile unsigned char *v=p;while(n--)*v++=0;}
static unsigned curve(ecc_key *key) {
    if(!key||!key->dp)return 0;
    if(key->dp->id==ECC_SECP256R1)return 256;
    if(key->dp->id==ECC_SECP384R1)return 384;
    if(key->dp->id==ECC_SECP521R1)return 521;
    return 0;
}
static int invoke(struct vm_crypto_request *p) {
    int fd,ret,saved;
    fd=open("/dev/crypto",O_RDWR);if(fd<0)return CRYPTOCB_UNAVAILABLE;
    ret=ioctl(fd,RIPPLE_CRYPTO_IOCTL,p);saved=errno;close(fd);
    if(ret<0)return saved==ENOSYS?CRYPTOCB_UNAVAILABLE:WC_HW_E;
    return 0;
}
static int copy_output(struct vm_crypto_request *p,byte *out,word32 *size) {
    if(p->out_len>*size)return BUFFER_E;
    memcpy(out,p->out,p->out_len);*size=p->out_len;return 0;
}
static int scalar(ecc_key *key,byte *out,unsigned n) {
    /* k holds the private scalar, including transient signing nonces. */
    return mp_to_unsigned_bin_len(key->k,out,n);
}
/* Complete records cross the device boundary. Non-TLS nonce/tag shapes use
 * wolfSSL's software implementation; a rejected tag never falls back. */
static int gcm_callback(wc_CryptoInfo *info) {
    struct vm_crypto_gcm_request *p;
    Aes *aes;
    byte *out,*tag_out=NULL;
    const byte *in,*iv,*aad,*tag_in=NULL;
    word32 size,iv_size,aad_size,tag_size;
    unsigned cells,operation;
    unsigned long start;
    int fd,ret,saved;
    if(!bulk_enabled||info->cipher.type!=WC_CIPHER_AES_GCM)return CRYPTOCB_UNAVAILABLE;
    if(info->cipher.enc) {
        wc_CryptoCb_AesAuthEnc *a=&info->cipher.aesgcm_enc;
        aes=a->aes;out=a->out;in=a->in;size=a->sz;iv=a->iv;iv_size=a->ivSz;
        aad=a->authIn;aad_size=a->authInSz;tag_out=a->authTag;tag_size=a->authTagSz;
        operation=VM_CRYPTO_AES_GCM_ENCRYPT;
    } else {
        wc_CryptoCb_AesAuthDec *a=&info->cipher.aesgcm_dec;
        aes=a->aes;out=a->out;in=a->in;size=a->sz;iv=a->iv;iv_size=a->ivSz;
        aad=a->authIn;aad_size=a->authInSz;tag_in=a->authTag;tag_size=a->authTagSz;
        operation=VM_CRYPTO_AES_GCM_DECRYPT;
    }
    if(!aes||(aes->keylen!=16&&aes->keylen!=32)||!WC_AES_KEY_IS_SET(aes)||
       aes->rounds!=(unsigned)(aes->keylen/4+6)||
       iv_size!=12||tag_size!=16||aad_size>64||size>VM_CRYPTO_GCM_MAX)return CRYPTOCB_UNAVAILABLE;
    cells=VM_CRYPTO_GCM_HEADER+(unsigned)size;
    p=malloc(cells);if(!p)return MEMORY_E;
    memset(p,0,VM_CRYPTO_GCM_HEADER);p->version=2;p->operation=operation;
    p->key_len=aes->keylen;p->aad_len=aad_size;p->data_len=size;
    memcpy(p->key,aes->devKey,p->key_len);memcpy(p->nonce,iv,12);
    if(aad_size)memcpy(p->aad,aad,aad_size);
    if(size)memcpy(p->data,in,size);
    if(tag_in)memcpy(p->tag,tag_in,16);
    start=stamp();fd=open("/dev/crypto",O_RDWR);
    if(fd<0)ret=CRYPTOCB_UNAVAILABLE;
    else {
        ret=ioctl(fd,RIPPLE_CRYPTO_GCM_IOCTL,p);saved=errno;close(fd);
        if(ret<0)ret=saved==ENOSYS||saved==ENOTTY?CRYPTOCB_UNAVAILABLE:
            saved==EACCES?AES_GCM_AUTH_E:WC_HW_E;
        else {
            if(size)memcpy(out,p->data,size);
            if(tag_out)memcpy(tag_out,p->tag,16);
        }
    }
    if(ret!=CRYPTOCB_UNAVAILABLE){calls[operation]++;milliseconds[operation]+=stamp()-start;}
    clear(p,cells);free(p);return ret;
}
static int callback(int id,wc_CryptoInfo *info,void *context) {
    struct vm_crypto_request p;
    ecc_key *key=NULL;
    unsigned size=0,i,hash_nonzero;
    word32 n,rlen,slen;
    unsigned long start;
    int ret=CRYPTOCB_UNAVAILABLE;
    (void)id;(void)context;
    if(info->algo_type==WC_ALGO_TYPE_CIPHER)return gcm_callback(info);
    if(info->algo_type!=WC_ALGO_TYPE_PK)return ret;
    memset(&p,0,sizeof(p));p.version=1;p.out_len=512;
    if(info->pk.type==WC_PK_TYPE_EC_KEYGEN)key=info->pk.eckg.key;
    else if(info->pk.type==WC_PK_TYPE_EC_MAKE_PUB)key=info->pk.ecc_make_pub.key;
    else if(info->pk.type==WC_PK_TYPE_ECDH)key=info->pk.ecdh.private_key;
    else if(info->pk.type==WC_PK_TYPE_ECDSA_VERIFY)key=info->pk.eccverify.key;
    if(key){p.curve=curve(key);if(!p.curve)return ret;size=key->dp->size;}
    start=stamp();
    if(info->pk.type==WC_PK_TYPE_EC_KEYGEN) {
        mp_int order;
        p.operation=VM_CRYPTO_EC_PUBLIC;p.a_len=size;
        ret=mp_init(&order);
        if(!ret)ret=mp_read_radix(&order,key->dp->order,16);
        if(!ret)ret=wc_ecc_gen_k(info->pk.eckg.rng,size,key->k,&order);
        mp_clear(&order);
        if(!ret)ret=scalar(key,p.a,size);
        if(!ret)ret=invoke(&p);
        if(!ret && (p.out_len!=2*size+1||p.out[0]!=4))ret=WC_HW_E;
        if(!ret)ret=mp_read_unsigned_bin(key->pubkey.x,p.out+1,size);
        if(!ret)ret=mp_read_unsigned_bin(key->pubkey.y,p.out+1+size,size);
        if(!ret)ret=mp_set(key->pubkey.z,1);
        if(!ret)key->type=ECC_PRIVATEKEY;
        else mp_forcezero(key->k);
    } else if(info->pk.type==WC_PK_TYPE_EC_MAKE_PUB) {
        p.operation=VM_CRYPTO_EC_PUBLIC;p.a_len=size;
        ret=scalar(key,p.a,size);
        if(!ret)ret=invoke(&p);
        if(!ret)ret=copy_output(&p,info->pk.ecc_make_pub.pubOut,info->pk.ecc_make_pub.pubOutSz);
    } else if(info->pk.type==WC_PK_TYPE_ECDH) {
        if(!info->pk.ecdh.public_key||curve(info->pk.ecdh.public_key)!=p.curve)goto done;
        p.operation=VM_CRYPTO_ECDH;p.a_len=size;n=512;
        ret=scalar(key,p.a,size);
        if(!ret)ret=wc_ecc_export_x963(info->pk.ecdh.public_key,p.b,&n);
        p.b_len=n;
        if(!ret)ret=invoke(&p);
        if(!ret)ret=copy_output(&p,info->pk.ecdh.out,info->pk.ecdh.outlen);
    } else if(info->pk.type==WC_PK_TYPE_ECDSA_VERIFY) {
        p.operation=VM_CRYPTO_ECDSA_VERIFY;n=512;
        *info->pk.eccverify.res=0;
#ifndef WC_ALLOW_ECC_ZERO_HASH
        /* Preserve wolfSSL's software validation before the hardware hook. */
        hash_nonzero=0;
        for(i=0;i<info->pk.eccverify.hashlen;i++)hash_nonzero|=info->pk.eccverify.hash[i];
        if(!hash_nonzero){ret=ECC_BAD_ARG_E;goto done;}
#endif
        ret=wc_ecc_export_x963(key,p.a,&n);p.a_len=n;
        if(info->pk.eccverify.hashlen>64){ret=BAD_FUNC_ARG;goto done;}
        p.b_len=info->pk.eccverify.hashlen;memcpy(p.b,info->pk.eccverify.hash,p.b_len);
        rlen=slen=size;
        if(!ret)ret=wc_ecc_sig_to_rs(info->pk.eccverify.sig,info->pk.eccverify.siglen,p.c,&rlen,p.c+size,&slen);
        if(!ret) {
            memmove(p.c+size-rlen,p.c,rlen);memset(p.c,0,size-rlen);
            memmove(p.c+2*size-slen,p.c+size,slen);memset(p.c+size,0,size-slen);
            p.c_len=2*size;ret=invoke(&p);
        }
        if(!ret){if(p.out_len!=1||p.out[0]>1)ret=WC_HW_E;else *info->pk.eccverify.res=p.out[0];}
    } else if(info->pk.type==WC_PK_TYPE_RSA) {
        RsaKey *rsa=info->pk.rsa.key;
        if(info->pk.rsa.type!=RSA_PUBLIC_ENCRYPT&&info->pk.rsa.type!=RSA_PUBLIC_DECRYPT)goto done;
        n=mp_unsigned_bin_size(&rsa->n);
        if(n!=256&&n!=384&&n!=512)goto done;
        p.operation=VM_CRYPTO_RSA_PUBLIC;
        if(info->pk.rsa.inLen>512){ret=BAD_FUNC_ARG;goto done;}
        p.a_len=info->pk.rsa.inLen;memcpy(p.a,info->pk.rsa.in,p.a_len);
        p.b_len=n;p.c_len=mp_unsigned_bin_size(&rsa->e);
        if(p.c_len>8)goto done;
        ret=mp_to_unsigned_bin(&rsa->n,p.b);
        if(!ret)ret=mp_to_unsigned_bin(&rsa->e,p.c);
        if(!ret)ret=invoke(&p);
        if(!ret)ret=copy_output(&p,info->pk.rsa.out,info->pk.rsa.outLen);
    }
done:
    if(p.operation&&ret!=CRYPTOCB_UNAVAILABLE){calls[p.operation]++;milliseconds[p.operation]+=stamp()-start;}
    clear(&p,sizeof(p));return ret;
}
static int find_device(int id,int algorithm){return algorithm==WC_ALGO_TYPE_PK||(bulk_enabled&&algorithm==WC_ALGO_TYPE_CIPHER)?73:id;}
void ripple_crypto_report(void) {
    unsigned i;
    if(!getenv("RIPPLE_TLS_PROFILE"))return;
    fprintf(stderr,"TLS crypto accelerator: %s\n",enabled?"enabled":"software");
    for(i=1;i<=14;i++)if(i<=4||i>=13)fprintf(stderr,"TLS crypto operation %u: %lu calls, %lu ms\n",i,calls[i],milliseconds[i]);
}
void ripple_crypto_init(void) {
    struct vm_crypto_request query;
    if(initialized)return;initialized=1;
    if(getenv("RIPPLE_TLS_SOFTWARE"))return;
    memset(&query,0,sizeof(query));query.version=1;query.out_len=512;
    if(invoke(&query)||query.out_len!=3||query.out[0]!=1||(query.out[1]&7)!=7||(query.out[2]&30)!=30)return;
    query.curve=1;query.out_len=512;
    if(!invoke(&query)&&query.out_len==3&&query.out[0]==1&&(query.out[2]&0x60)==0x60)bulk_enabled=1;
    clear(&query,sizeof(query));
    if(wc_CryptoCb_RegisterDevice(73,callback,NULL))return;
    wc_CryptoCb_SetDeviceFindCb(find_device);enabled=1;
}
