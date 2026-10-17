/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <errno.h>
#include <pwd.h>
#include <string.h>
#include <sys/utsname.h>
#include <unistd.h>

int gethostname(char *name, size_t size) {
    struct utsname system;
    if (!name) { errno = EFAULT; return -1; }
    if (uname(&system) < 0) return -1;
    if (size <= strlen(system.nodename)) { errno = ENAMETOOLONG; return -1; }
    strcpy(name, system.nodename);
    return 0;
}

/* This port has no utmp login records yet. Use its account database. */
int getlogin_r(char *name, size_t size) {
    struct passwd *account;
    if (!name) return EFAULT;
    account = getpwuid(getuid());
    if (!account) return ENOENT;
    if (size <= strlen(account->pw_name)) return ERANGE;
    strcpy(name, account->pw_name);
    return 0;
}
char *getlogin(void) {
    static char name[32];
    int error = getlogin_r(name, sizeof(name));
    if (error) { errno = error; return NULL; }
    return name;
}
