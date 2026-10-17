/* SPDX-License-Identifier: GPL-2.0-or-later */
/* GUI clients keep their PID/job identity, but do not retain their launching
 * terminal. Redirected files/pipes remain useful for diagnostics and tests. */
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <stdlib.h>
#include <termios.h>
static int nxh_is_terminal(int fd) {struct termios t;return tcgetattr(fd,&t)==0;}
static void nxh_detach_terminal(void) {
    int fd,i,needed=0;
    const char *keep=getenv("NANOX_KEEP_TTY");
    if(keep&&*keep)return;
    signal(SIGHUP,SIG_IGN);
    for(i=0;i<3;i++)if(nxh_is_terminal(i))needed=1;
    if(!needed)return;
    fd=open("/dev/null",O_RDWR);if(fd<0)return;
    for(i=0;i<3;i++)if(nxh_is_terminal(i))dup2(fd,i);
    if(fd>2)close(fd);
}
