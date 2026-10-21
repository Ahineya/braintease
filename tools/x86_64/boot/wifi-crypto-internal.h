/* Freestanding adapter for pinned OpenBSD SHA1/rijndael; algorithm unchanged. */
#ifndef RIPLIX_WIFI_CRYPTO_INTERNAL_H
#define RIPLIX_WIFI_CRYPTO_INTERNAL_H
#include "wifi-crypto.h"
void *memcpy(void *,const void *,size_t);
void *memset(void *,int,size_t);
_Static_assert(__BYTE_ORDER__==__ORDER_LITTLE_ENDIAN__,"native LE targets only");
#define htobe32(v) __builtin_bswap32(v)
#define htobe64(v) __builtin_bswap64(v)
#define be32toh(v) __builtin_bswap32(v)
#define explicit_bzero wifi_wipe
#define SHA1_BLOCK_LENGTH 64
#define SHA1_DIGEST_LENGTH 20
typedef struct {uint32_t state[5];uint64_t count;uint8_t buffer[64];} SHA1_CTX;
void SHA1Init(SHA1_CTX *);
void SHA1Update(SHA1_CTX *,const uint8_t *,size_t);
void SHA1Final(uint8_t[20],SHA1_CTX *);
typedef unsigned char u_char;
#include "wifi-rijndael.h"
#endif
