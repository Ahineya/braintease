/* Reviewed cross-target configuration for the initial C16 feasibility build.
 * sizeof is measured in target C characters (16-bit), never host octets.
 * No host configure result, ncurses binary, TLS or host socket is imported. */
#ifndef LYNX_CFG_H
#define LYNX_CFG_H 1
#include <sys/select.h>
#include <pwd.h>
#define UNIX 1
#define SYSTEM_NAME "FUZIX C16"
#define SIZEOF_INT 1
#define SIZEOF_LONG 2
#define SIZEOF_OFF_T 2
#define SIZEOF_SIZE_T 2
#define SIZEOF_TIME_T 2
#define ALL_CHARSETS 0
#define NO_CHARSET_iso_8859_1 0
#define NO_CHARSET_us_ascii 0
#define NO_LYNX_TRACE 1
#define NO_CONFIG_INFO 1
#define NO_OPTION_MENU 1
#define NO_OPTION_FORMS 1
#define DISABLE_BIBP 1
#define DISABLE_FINGER 1
#define DISABLE_FTP 1
#define DISABLE_GOPHER 1
#define DISABLE_NEWS 1
#define DONT_HAVE_TM_GMTOFF 1
#define DONT_HAVE_PW_GECOS 1
#define CAN_SET_ERRNO 1
#define HAVE_ARPA_INET_H 1
#define HAVE_DIRENT_H 1
#define HAVE_FCNTL_H 1
#define HAVE_GETCWD 1
#define HAVE_GETUID 1
#define HAVE_GETPWUID 1
#define HAVE_PWD_H 1
#define HAVE_PUTENV 1
#define HAVE_H_ERRNO 1
#define HAVE_INET_ATON 1
#define HAVE_LIMITS_H 1
#define HAVE_MKDTEMP 1
#define HAVE_MKTIME 1
#define HAVE_READDIR 1
#define HAVE_SLEEP 1
#define HAVE_STDARG_H 1
#define HAVE_STDINT_H 1
#define HAVE_STDLIB_H 1
#define HAVE_STRERROR 1
#define HAVE_STRING_H 1
#define HAVE_SYS_IOCTL_H 1
#define HAVE_SYS_STAT_H 1
#define HAVE_SYS_TIME_H 1
#define HAVE_SYS_TYPES_H 1
#define HAVE_SYS_WAIT_H 1
#define HAVE_TERMIOS_H 1
#define HAVE_UNISTD_H 1
#define HAVE_USLEEP 1
#define HAVE_WAITPID 1
#define HAVE_CBREAK 1
#define HAVE_KEYPAD 1
#define HAVE_TOUCHWIN 1
#define HAVE_TYPE_CHTYPE 1
#define HAVE_RESIZETERM 1
#define HAVE_SIZECHANGE 1
#define TIME_WITH_SYS_TIME 1
#define STDC_HEADERS 1
#define UNDERLINE_LINKS 0
#define USE_FCNTL 1
#define LYNX_RAND_MAX 32767
#define LYNX_CFG_FILE "/etc/lynx.cfg"
#define LYNX_CFG_PATH "/etc"
#define LYNX_LSS_FILE "/etc/lynx.lss"
#define MIME_LIBDIR "/etc"
#define SYSTEM_MAIL ""
#define SYSTEM_MAIL_FLAGS ""
#define LOCALEDIR "/usr/share/locale"
#define lstat stat
/* off_t is a signed long. Upstream's fallback lives in disabled FTP code. */
#define LYatoll atol
#endif
