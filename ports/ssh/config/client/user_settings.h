#ifndef RIPLIX_SSH_CLIENT_SETTINGS_H
#define RIPLIX_SSH_CLIENT_SETTINGS_H
#include "../user_settings.h"
#undef NO_WOLFSSH_CLIENT
/* CBC is used only for encrypted private-key files, never SSH transport. */
#undef NO_AES_CBC
#define WOLFSSL_BASE64_ENCODE
#define WOLFSSL_DER_TO_PEM
#define HAVE_SYS_IOCTL_H
#endif
