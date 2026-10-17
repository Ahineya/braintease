/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Bootstrap PID 1 into the upstream shell without using another task slot. */
#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>
int main(void) {
    char *args[] = {"sh", NULL};
    char *env[] = {"PATH=/bin", "HOME=/", "USER=root", "TERM=ansi",
                   "PS1=fuzix# ", "SHELL=/bin/sh", "EDITOR=/bin/vi", NULL};
    if (open("/dev/tty1", O_RDWR) != 0 || dup(0) != 1 || dup(0) != 2)
        return 1;
    execve("/bin/sh", args, env);
    perror("/bin/sh");
    uadmin(1, 0, NULL);
    return 1;
}
