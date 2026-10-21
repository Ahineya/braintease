#ifndef RIPLIX_SSH_SETTINGS_H
#define RIPLIX_SSH_SETTINGS_H
/* Separate crypto-only profile: existing HTTPS configuration is unchanged. */
#include "../../tls/config/user_settings.h"
#define WOLFCRYPT_ONLY
#define WOLFSSL_WOLFSSH
#define NO_WOLFSSH_CLIENT
#define WOLFSSH_USER_IO
#define WOLFSSH_TERM
#define WOLFSSH_SHELL
#define WOLFSSH_SFTP
#undef NO_FILESYSTEM
#undef NO_WOLFSSL_DIR
#define WOLFSSH_MAX_SFTP_RW 16384UL
#define WOLFSSH_MAX_SFTP_RECV 32768UL
#define WOLFSSH_MAX_SFTP_PACKET 65536UL
#define WOLFSSH_MAX_SFTP_NAME 65536UL
/* Keep RSA-SHA2 signatures; never negotiate legacy SHA1 ssh-rsa signatures. */
#define WOLFSSH_NO_SSH_RSA_SHA1
#undef WOLFSSL_RSA_PUBLIC_ONLY
#undef WOLFSSL_RSA_VERIFY_ONLY
#define WC_RSA_BLINDING
#define WOLFSSL_KEY_GEN
#define HAVE_ED25519
#define ED25519_SMALL
#define HAVE_ED25519_MAKE_KEY
#define HAVE_ED25519_SIGN
#define HAVE_ED25519_VERIFY
#define HAVE_ED25519_KEY_IMPORT
#define HAVE_ED25519_KEY_EXPORT
#define WOLFSSL_ED25519_STREAMING_VERIFY
#define DEFAULT_WINDOW_SZ 16384UL
#define DEFAULT_MAX_PACKET_SZ 16384UL
/* Keep the RFC-required transport receive size. Socket callbacks chunk I/O. */
#define MAX_PACKET_SZ 35000UL
#undef CUSTOM_RAND_GENERATE_SEED
#define CUSTOM_RAND_GENERATE_SEED ripple_ssh_seed
extern int ripple_ssh_seed(unsigned char *, unsigned long);
#endif
