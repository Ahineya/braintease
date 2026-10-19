/* SPDX-License-Identifier: MIT */
#ifndef FUSE_TELEGRAM_CORE_H
#define FUSE_TELEGRAM_CORE_H
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
typedef unsigned char TgByte;
typedef uint32_t TgU32;
typedef uint64_t TgU64;
#define TG_MAX 28672
typedef struct { TgByte *p; unsigned n, at; int ok; } TgTl;
typedef struct { TgU32 h[8], total; unsigned used; int sha1; TgByte block[64]; } TgHash;
void tg_erase(void *, unsigned long);
int tg_equal(const TgByte *, const TgByte *, unsigned);
TgU32 tg_le32(const TgByte *);
TgU64 tg_le64(const TgByte *);
void tg_put32(TgByte *, TgU32);
void tg_put64(TgByte *, TgU64);
void tg_hash_init(TgHash *, int);
void tg_hash_add(TgHash *, const TgByte *, unsigned);
void tg_hash_end(TgHash *, TgByte *);
void tg_hash(int, const TgByte *, unsigned, TgByte *);
void tg_tl_init(TgTl *, TgByte *, unsigned);
TgU32 tg_u32(TgTl *);
TgU64 tg_u64(TgTl *);
const TgByte *tg_raw(TgTl *, unsigned);
const TgByte *tg_bytes(TgTl *, unsigned *);
void tg_wraw(TgTl *, const TgByte *, unsigned);
void tg_w32(TgTl *, TgU32);
void tg_w64(TgTl *, TgU64);
void tg_wbytes(TgTl *, const TgByte *, unsigned);
void tg_wtext(TgTl *, const char *);
int tg_gunzip(const TgByte *, unsigned, TgByte *, unsigned);
int tg_random(TgByte *, unsigned);
int tg_ige(TgByte *, unsigned, const TgByte *, const TgByte *, int);
int tg_modexp(const TgByte *, unsigned, const TgByte *, const TgByte *, unsigned, TgByte *);
int tg_modmul(const TgByte *, const TgByte *, const TgByte *, TgByte *);
int tg_group(const TgByte *, unsigned);
int tg_good_public(const TgByte *, const TgByte *);
int tg_srp(const TgByte *, unsigned, const TgByte *, unsigned, const TgByte *, unsigned,
           unsigned, const TgByte *, const TgByte *, TgByte *, TgByte *);
extern const TgByte tg_prime[256];
extern const TgByte tg_rsa_n[2][256], tg_rsa_fingerprint[2][8];
/* Stateless guest facade, provided by rfl/os/crypto.c or the host test shim. */
long rfl_crypto_call(unsigned,unsigned,const TgByte *,unsigned long,const TgByte *,unsigned long,
                     const TgByte *,unsigned long,TgByte *,unsigned long);
int rfl_crypto_random(TgByte *,unsigned long);
#endif
