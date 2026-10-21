#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../boot/wifi-crypto.h"
#include "fixtures/wifi-wpa-vectors.h"
static void hex(uint8_t *p,const char *s){for(unsigned i=0;s[2*i];i++){unsigned v;assert(sscanf(s+2*i,"%2x",&v)==1);p[i]=v;}}
static void expected(const uint8_t *p,unsigned n,const char *s){uint8_t v[64];assert(n<=64);hex(v,s);assert(!memcmp(p,v,n));}
int main(void) {
 uint8_t result[64],key[80],data[64];
 wifi_sha1((const uint8_t *)"",0,result);expected(result,20,"da39a3ee5e6b4b0d3255bfef95601890afd80709");
 wifi_sha1((const uint8_t *)"abc",3,result);expected(result,20,"a9993e364706816aba3e25717850c26c9cd0d89d");
 memset(key,0x0b,20);wifi_hmac_sha1(key,20,(const uint8_t *)"Hi There",8,result);expected(result,20,"b617318655057264e28bc0b6fb378c8ef146be00");
 memset(key,0xaa,80);const char *longtext="Test Using Larger Than Block-Size Key - Hash Key First";
 wifi_hmac_sha1(key,80,(const uint8_t *)longtext,strlen(longtext),result);expected(result,20,"aa4ae5e15272d00e95705637ce8a3b55ed402112");
 assert(wifi_pmk((const uint8_t *)"password",8,(const uint8_t *)"IEEE",4,result));expected(result,32,"f42c6fc52df0ebef9ebb4b90b38a5f902e83fe1b135a70e23aed762e9710a12e");
 memset(result,0x5a,32);assert(!wifi_pmk((const uint8_t *)"short",5,(const uint8_t *)"IEEE",4,result));for(unsigned i=0;i<32;i++)assert(result[i]==0x5a);
 wifi_ptk(vector_pmk,vector_ap,vector_own,vector_anonce,vector_snonce,result);assert(!memcmp(result,vector_ptk,48));
 wifi_ptk(vector_pmk,vector_own,vector_ap,vector_snonce,vector_anonce,result);assert(!memcmp(result,vector_ptk,48));
 hex(key,"000102030405060708090a0b0c0d0e0f");hex(data,"1fa68b0a8112b447aef34bd8fb5a7b829d3e862371d2cfe5");
 assert(wifi_unwrap(key,data,24,result));expected(result,16,"00112233445566778899aabbccddeeff");
 for(unsigned i=0;i<24;i++){data[i]^=1;memset(result,0x5a,sizeof(result));assert(!wifi_unwrap(key,data,24,result));for(unsigned j=0;j<64;j++)assert(result[j]==0x5a);data[i]^=1;}
 assert(wifi_unwrap(key,data,24,data));expected(data,16,"00112233445566778899aabbccddeeff");
 for(unsigned n=0;n<24;n++)assert(!wifi_unwrap(key,data,n,result));
 wifi_wipe(result,sizeof(result));for(unsigned n=0;n<sizeof(result);n++)assert(!result[n]);
 puts("AX211_CRYPTO_HOST_OK SHA1/HMAC/PBKDF2/PTK/RFC3394 public vectors, tamper and atomic rejection");
}
