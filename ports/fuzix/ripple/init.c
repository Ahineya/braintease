/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Bootstrap PID 1 into the upstream shell without using another task slot. */
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#define RH_OPEN(p) open(p,O_RDONLY)
#define RH_READ(fd,p,n) read(fd,p,n)
#define RH_CLOSE(fd) close(fd)
#define RH_CHDIR(p) chdir(p)
#include "root-home.h"
int main(void) {
    char *args[] = {"sh", NULL};
    char *env[] = {"PATH=/usr/bin:/bin", root_home_environment, "USER=root", "TERM=ansi",
                   "PS1=riplix# ", "SHELL=/bin/sh", "EDITOR=/bin/vi", "LOGNAME=root", NULL};
    if (open("/dev/tty1", O_RDWR) != 0 || dup(0) != 1 || dup(0) != 2)
        return 1;
    if(root_home_start()<0)fputs("init: home unavailable; starting in /\n",stderr);
    execve("/bin/sh", args, env);
    perror("/bin/sh");
    uadmin(1, 0, NULL);
    return 1;
}
