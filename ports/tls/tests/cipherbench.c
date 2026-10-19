/* Fixed public fixtures only; this is a throughput probe, not an encryption tool. */
#include <wolfssl/ssl.h>
#include <ripple_crypto.h>
#include <wolfssl/wolfcrypt/aes.h>
#include <wolfssl/wolfcrypt/error-crypt.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
static byte input[4096], cipher[4096], output[4096], key[16], iv[12], tag[16];
static Aes aes;
static unsigned long now(void) {struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return (unsigned long)t.tv_sec*1000UL+t.tv_nsec/1000000UL;}
int main(void) {
 unsigned i,j;unsigned long start,elapsed;int r;
 if(wolfSSL_Init()!=WOLFSSL_SUCCESS)return 1;
 ripple_crypto_init();
 for(i=0;i<4096;i++)input[i]=i&255;
 if(wc_AesInit(&aes,NULL,INVALID_DEVID)||wc_AesGcmSetKey(&aes,key,16))return 1;
 start=now();
 for(j=0;j<8;j++){iv[0]=j;r=wc_AesGcmEncrypt(&aes,cipher,input,4096,iv,12,tag,16,NULL,0);if(r)return 2;}
 elapsed=now()-start;printf("AES128-GCM encrypt 32768 octets: %lu ms\n",elapsed);
 start=now();
 for(j=0;j<8;j++){r=wc_AesGcmDecrypt(&aes,output,cipher,4096,iv,12,tag,16,NULL,0);if(r||memcmp(input,output,4096))return 3;}
 elapsed=now()-start;printf("AES128-GCM decrypt 32768 octets: %lu ms\n",elapsed);
 tag[0]^=1;r=wc_AesGcmDecrypt(&aes,output,cipher,4096,iv,12,tag,16,NULL,0);
 if(r!=AES_GCM_AUTH_E)return 4;
 wc_AesFree(&aes);ripple_crypto_report();puts("cipherbench passed");return 0;
}
