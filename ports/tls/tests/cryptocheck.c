#include <wolfssl/wolfcrypt/hash.h>
#include <wolfssl/wolfcrypt/error-crypt.h>
#include <wolfssl/wolfcrypt/hmac.h>
#include <wolfssl/wolfcrypt/aes.h>
#include <wolfssl/wolfcrypt/random.h>
#include <wolfssl/wolfcrypt/ecc.h>
#include <wolfssl/wolfcrypt/rsa.h>
#include <wolfssl/wolfcrypt/signature.h>
#include <stdio.h>
#include <time.h>
#include <wolfssl/ssl.h>
#include "ripple_crypto.h"
#include <string.h>
#include "crypto-vectors.h"
#include "pk-vectors.h"
static unsigned long clock_ms(void) {struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return (unsigned long)t.tv_sec*1000UL+t.tv_nsec/1000000UL;}
static int hashes(void) {
    unsigned char output[66], key[20];
    Hmac h;
    output[64]=0x1234;output[65]=0x5678;
    if(wc_Hash(WC_HASH_TYPE_SHA, (const byte *)"abc",3,output,20)||memcmp(output,sha1_abc,20))return 1;
    if(wc_Hash(WC_HASH_TYPE_SHA256,(const byte *)"abc",3,output,32)||memcmp(output,sha256_abc,32))return 2;
    if(wc_Hash(WC_HASH_TYPE_SHA384,(const byte *)"abc",3,output,48)||memcmp(output,sha384_abc,48))return 3;
    if(wc_Hash(WC_HASH_TYPE_SHA512,(const byte *)"abc",3,output,64)||memcmp(output,sha512_abc,64))return 4;
    if(output[64]!=0x1234||output[65]!=0x5678)return 5;
    memset(key,0x0b,sizeof(key));
    if(wc_HmacInit(&h,NULL,INVALID_DEVID)||wc_HmacSetKey(&h,WC_SHA256,key,sizeof(key))||
       wc_HmacUpdate(&h,(const byte *)"Hi There",8)||wc_HmacFinal(&h,output)||memcmp(output,hmac256,32))return 6;
    wc_HmacFree(&h);
    return 0;
}
static int aes_gcm(void) {
    Aes aes;
    byte key[32],iv[12],plain[16],cipher[18],tag[16],decoded[16];
    byte longer[62],restored[60];
    int ret;
    memset(key,0,sizeof(key));memset(iv,0,sizeof(iv));memset(plain,0,sizeof(plain));
    cipher[16]=0x1234;cipher[17]=0x5678;
    if(wc_AesInit(&aes,NULL,INVALID_DEVID)||wc_AesGcmSetKey(&aes,key,16)||
       wc_AesGcmEncrypt(&aes,cipher,plain,16,iv,12,tag,16,NULL,0)||
       memcmp(cipher,gcm128_cipher,16)||memcmp(tag,gcm128_tag,16))return 1;
    if(wc_AesGcmDecrypt(&aes,decoded,cipher,16,iv,12,tag,16,NULL,0)||memcmp(decoded,plain,16))return 2;
    tag[0]^=1;
    ret=wc_AesGcmDecrypt(&aes,decoded,cipher,16,iv,12,tag,16,NULL,0);
    if(ret!=AES_GCM_AUTH_E)return 3;
    if(wc_AesGcmSetKey(&aes,key,32)||wc_AesGcmEncrypt(&aes,cipher,plain,16,iv,12,tag,16,NULL,0)||
       memcmp(cipher,gcm256_cipher,16)||memcmp(tag,gcm256_tag,16))return 4;
    if(cipher[16]!=0x1234||cipher[17]!=0x5678)return 5;
    longer[60]=0x1234;longer[61]=0x5678;
    if(wc_AesGcmSetKey(&aes,gcm_aad_key,16)||wc_AesGcmEncrypt(&aes,longer,gcm_aad_plain,60,gcm_aad_iv,12,tag,16,gcm_aad,20)||
       memcmp(longer,gcm_aad_cipher,60)||memcmp(tag,gcm_aad_tag,16))return 6;
    if(wc_AesGcmDecrypt(&aes,restored,longer,60,gcm_aad_iv,12,tag,16,gcm_aad,20)||memcmp(restored,gcm_aad_plain,60))return 7;
    if(longer[60]!=0x1234||longer[61]!=0x5678)return 8;
    wc_AesFree(&aes);
    return 0;
}
static int extended(WC_RNG *rng) {
    byte data[333], key[131], out[68], exported[133], ikm[22], salt[13], info[10];
    unsigned i;
    int ret;
    Hmac h;
    ecc_key first,second;
    word32 size=64;
    for(i=0;i<333;i++)data[i]=i&255;
    for(i=0;i<131;i++)key[i]=i;
    if(wc_Hash(WC_HASH_TYPE_SHA256,data,333,out,32)||memcmp(out,sha256_long,32))return 1;
    if(wc_Hash(WC_HASH_TYPE_SHA384,data,333,out,48)||memcmp(out,sha384_long,48))return 2;
    if(wc_Hash(WC_HASH_TYPE_SHA512,data,333,out,64)||memcmp(out,sha512_long,64))return 3;
    if(wc_HmacInit(&h,NULL,INVALID_DEVID)||wc_HmacSetKey(&h,WC_SHA384,key,131)||wc_HmacUpdate(&h,data,333)||wc_HmacFinal(&h,out)||memcmp(out,sha384_hmac,48))return 4;
    wc_HmacFree(&h);
    memset(ikm,11,22);for(i=0;i<13;i++)salt[i]=i;for(i=0;i<10;i++)info[i]=0xf0+i;
    if(wc_HKDF(WC_SHA256,ikm,22,salt,13,info,10,out,42)||memcmp(out,hkdf256,42))return 5;
    {
        const byte *private_key[3]={ec_private_1,ec384_private_1,ec521_private_1};
        const byte *public_key[3]={ec_public_1,ec384_public_1,ec521_public_1};
        const byte *other[3]={ec_public_2,ec384_public_2,ec521_public_2};
        const byte *expected[3]={ec_shared,ec384_shared,ec521_shared};
        const byte *wide[3]={ec_private_wide,ec384_private_wide,ec521_private_wide};
        const byte *wide_public[3]={ec_public_wide,ec384_public_wide,ec521_public_wide};
        const byte *wide_expected[3]={ec_shared_wide,ec384_shared_wide,ec521_shared_wide};
        unsigned sizes[3]={32,48,66},j;
        for(j=0;j<3;j++) {
            unsigned long started=clock_ms();
            size=66;out[66]=0x1234;out[67]=0x5678;
            if(wc_ecc_init(&first)||wc_ecc_init(&second))return 6;
            if(wc_ecc_import_private_key(private_key[j],sizes[j],public_key[j],2*sizes[j]+1,&first)||
               wc_ecc_import_x963(other[j],2*sizes[j]+1,&second))return 7;
            { byte zero_hash[32]={0};const byte signature[8]={0x30,6,2,1,1,2,1,1};int valid=0;
              if(wc_ecc_verify_hash(signature,8,zero_hash,32,&valid,&first)!=ECC_BAD_ARG_E)return 12;
            }
            wc_ecc_set_rng(&first,rng);
            ret=wc_ecc_shared_secret(&first,&second,out,&size);
            wc_ecc_free(&second);wc_ecc_free(&first);
            if(ret||size!=sizes[j]||memcmp(out,expected[j],size)||out[66]!=0x1234||out[67]!=0x5678) {
                printf("ECDH curve %u mismatch ret=%d size=%lu\n",sizes[j],ret,size);return 8;
            }
            size=66;
            if(wc_ecc_init(&first)||wc_ecc_init(&second)||
               wc_ecc_import_private_key(wide[j],sizes[j],NULL,0,&first)||
               wc_ecc_import_x963(other[j],2*sizes[j]+1,&second))return 9;
            wc_ecc_set_rng(&first,rng);
            ret=wc_ecc_make_pub(&first,NULL);
            { word32 length=sizeof(exported);
              if(ret||wc_ecc_export_x963(&first,exported,&length)||length!=2*sizes[j]+1||
                 memcmp(exported,wide_public[j],length)) {
                  printf("ECC full-width public curve %u mismatch ret=%d\n",sizes[j],ret);return 10;
              }
            }
            ret=wc_ecc_shared_secret(&first,&second,out,&size);
            wc_ecc_free(&second);wc_ecc_free(&first);
            if(ret||size!=sizes[j]||memcmp(out,wide_expected[j],size)||out[66]!=0x1234||out[67]!=0x5678) {
                printf("ECDH full-width curve %u mismatch ret=%d\n",sizes[j],ret);return 11;
            }
            printf("TLS ECC curve %u public/ECDH vectors: %lu ms\n",sizes[j],clock_ms()-started);
        }
    }
    return 0;
}
int main(void) {
    WC_RNG rng;
    byte a[64],b[64];
    int ret;
    if(wolfSSL_Init()!=WOLFSSL_SUCCESS)return 9;
    ripple_crypto_init();
    ret=hashes();if(ret){printf("hash check failed %d\n",ret);return 1;}
    puts("TLS hash and HMAC known answers passed");
    ret=aes_gcm();if(ret){printf("AES-GCM check failed %d\n",ret);return 2;}
    puts("TLS AES-GCM known answers and rejected tag passed");
    ret=wc_InitRng(&rng);
    if(ret){printf("RNG seed failed %d\n",ret);return 3;}
    if(wc_RNG_GenerateBlock(&rng,a,64)||wc_RNG_GenerateBlock(&rng,b,64)||!memcmp(a,b,64))return 4;
    { unsigned i; for(i=0;i<64;i++)if(a[i]>255||b[i]>255)return 5; }
    ret=extended(&rng);
    if(ret){printf("TLS extended check failed %d\n",ret);return 6;}
    puts("TLS long hashes, HMAC-SHA384, HKDF and ECDH known answers passed");
    wc_FreeRng(&rng);
    puts("TLS host-seeded DRBG passed");
    ripple_crypto_report();
    puts("cryptocheck passed");
    return 0;
}
