#ifndef VM_CRYPTO_H
#define VM_CRYPTO_H
#include <limits.h>
/* Crypto device ABI 1. One octet per C16 cell, big-endian integers and
 * uncompressed SEC1 points. No host key handles, TLS sessions or trust policy. */
#define VM_DEVICE_CRYPTO 13
#define VM_CRYPTO_QUERY 0
#define VM_CRYPTO_EC_PUBLIC 1
#define VM_CRYPTO_ECDH 2
#define VM_CRYPTO_ECDSA_VERIFY 3
#define VM_CRYPTO_RSA_PUBLIC 4
#define VM_CRYPTO_MODEXP_2048 5
#define VM_CRYPTO_PBKDF2_SHA512 6
#define VM_CRYPTO_AES256_IGE_ENCRYPT 7
#define VM_CRYPTO_AES256_IGE_DECRYPT 8
#define VM_CRYPTO_SHA256 9
#define VM_CRYPTO_SHA512 10
#define VM_CRYPTO_MODMUL_2048 11
#define VM_CRYPTO_FACTOR_63 12
#define VM_CRYPTO_AES_GCM_ENCRYPT 13
#define VM_CRYPTO_AES_GCM_DECRYPT 14
#define VM_CRYPTO_MAX 512
#define RIPPLE_CRYPTO_IOCTL 0x4370
#define RIPPLE_CRYPTO_GCM_IOCTL 0x4371
#define VM_CRYPTO_GCM_MAX 18432
#define VM_CRYPTO_GCM_HEADER 130
/* ABI 2: one bounded record, in-place data and a detached 128-bit tag.
 * The host retains no key, counter, TLS state or unauthenticated plaintext.
 * Submit only HEADER + data_len cells, including for empty GMAC records. */
struct vm_crypto_gcm_request {
    unsigned short version, operation, key_len, aad_len, data_len, reserved;
    unsigned char key[32], nonce[12], tag[16], aad[64];
    unsigned char data[VM_CRYPTO_GCM_MAX];
};
struct vm_crypto_request {
    unsigned short version, operation, curve;
    unsigned short a_len, b_len, c_len, out_len, reserved;
    unsigned char a[VM_CRYPTO_MAX], b[VM_CRYPTO_MAX], c[VM_CRYPTO_MAX], out[VM_CRYPTO_MAX];
};
#if CHAR_BIT == 16
typedef char VmCryptoRequestCells[(sizeof(struct vm_crypto_request)==2056)?1:-1];
typedef char VmCryptoGcmCells[(sizeof(struct vm_crypto_gcm_request)==VM_CRYPTO_GCM_HEADER+VM_CRYPTO_GCM_MAX)?1:-1];
#endif
unsigned short vm_crypto(struct vm_crypto_request *request);
unsigned short vm_crypto_gcm(struct vm_crypto_gcm_request *request);
#endif
