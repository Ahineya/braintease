#ifndef RIPPLE_WOLFSSL_SETTINGS_H
#define RIPPLE_WOLFSSL_SETTINGS_H
/* Guest C16 TLS, with an optional stateless crypto coprocessor. */
#define WOLF_CRYPTO_CB
#define WOLF_CRYPTO_CB_FIND
#define WC_16BIT_CPU
#define WOLFSSL_GENERAL_ALIGNMENT 2
#define HAVE_LIMITS_H
#define WOLFSSL_NO_ASM
#define NO_INLINE
#define SINGLE_THREADED
#define SIZEOF_LONG 4
#define SIZEOF_LONG_LONG 8
#define WOLFSSL_USE_ALIGN
#define WOLFSSL_SMALL_STACK
#define NO_WRITEV
#define WOLFSSL_USER_IO
#define NO_WOLFSSL_SERVER
#define NO_FILESYSTEM
#define NO_WOLFSSL_DIR
#define NO_DEV_RANDOM
#define NO_WOLFSSL_MEMORY
#define CUSTOM_RAND_GENERATE_SEED ripple_tls_seed
#define NO_OLD_TLS
#define NO_DH
#define NO_DSA
#define NO_DES3
#define NO_RC4
#define NO_MD4
#define NO_MD5
#define NO_PWDBASED
#define NO_PKCS7
#define NO_PKCS12
#define NO_PSK
#define WOLFSSL_NO_ERROR_STRINGS
#define WOLFSSL_SHA384
#define WOLFSSL_SHA512
#define NO_AES_CBC
#define HAVE_AESGCM
#define GCM_SMALL
#define HAVE_ECC
#define ECC_USER_CURVES
#define HAVE_ECC256
#define HAVE_ECC384
#define HAVE_ECC521
#define HAVE_ECC_VERIFY
#define HAVE_ECC_DHE
#define HAVE_ECC_SIGN
#define ECC_TIMING_RESISTANT
#define WOLFSSL_SP_MATH
#define WOLFSSL_HAVE_SP_RSA
#define WOLFSSL_HAVE_SP_ECC
#define WOLFSSL_SP_ALLOW_16BIT_CPU
#define SP_WORD_SIZE 32
#define WOLFSSL_SP_384
#define WOLFSSL_SP_521
/* Compact digit arithmetic keeps spill frames within one RVM stack bank.
 * adapt.py independently selects upstream's precomputed ECC base tables. */
#define WOLFSSL_SP_SMALL
#define WOLFSSL_RSA_PUBLIC_ONLY
#define WOLFSSL_RSA_VERIFY_ONLY
#define WOLFSSL_SP_4096
#define HAVE_TLS_EXTENSIONS
#define HAVE_SNI
#define WOLFSSL_IP_ALT_NAME
#define WOLFSSL_HOSTNAME_VERIFY_ALT_NAME_ONLY
/* Prefer a path to a local trust anchor over an unrelated cross-sign sent
 * by the peer. The selected leaf still has to verify to that anchor. */
#define WOLFSSL_ALT_CERT_CHAINS
#define HAVE_SUPPORTED_CURVES
#define HAVE_EXTENDED_MASTER
#define WC_RSA_PSS
#define WOLFSSL_TLS13
#define HAVE_HKDF
#include <limits.h>
#include <time.h>
typedef char ripple_tls_char_bits[(CHAR_BIT==16)?1:-1];
typedef char ripple_tls_long_bits[(sizeof(long)*CHAR_BIT==32)?1:-1];
typedef char ripple_tls_wide_bits[(sizeof(long long)*CHAR_BIT==64)?1:-1];
typedef char ripple_tls_time_bits[(sizeof(time_t)*CHAR_BIT==32)?1:-1];
extern int ripple_tls_seed(unsigned char *, unsigned long);
#endif
