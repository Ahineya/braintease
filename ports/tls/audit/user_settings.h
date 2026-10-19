/* Compile-only feasibility profile, NOT a TLS-enabled library configuration.
 * No RNG implementation is linked, and this profile must never be shipped as
 * a downloader. The custom-seed declaration prevents default PRNG selection.
 * SIZEOF_LONG values follow wolfSSL's configuration convention, in octets. */
#define WOLFCRYPT_ONLY
#define WC_16BIT_CPU
#define WOLFSSL_GENERAL_ALIGNMENT 2
#define HAVE_LIMITS_H
#define WOLFSSL_NO_ASM
#define NO_INLINE
#define SINGLE_THREADED
#define SIZEOF_LONG 4
#define SIZEOF_LONG_LONG 8
#define NO_FILESYSTEM
#define NO_WOLFSSL_DIR
#define NO_DEV_RANDOM
#define NO_WOLFSSL_MEMORY
#define WOLFSSL_USER_CURRTIME
#define NO_ASN_TIME
#define NO_ASN
#define NO_CERTS
#define NO_RSA
#define NO_DH
#define NO_DSA
#define NO_AES
#define NO_DES3
#define NO_RC4
#define NO_MD4
#define NO_MD5
#define NO_SHA
#define NO_PWDBASED
#define NO_SIG_WRAPPER
#define CUSTOM_RAND_GENERATE_SEED ripple_seed

#define WOLFSSL_SP_ALLOW_16BIT_CPU
#define SP_WORD_SIZE 32
