/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef RIPPLE_SYS_UN_H
#define RIPPLE_SYS_UN_H
#include <sys/socket.h>
struct sockaddr_un {
    sa_family_t sun_family;
    char sun_path[RIPPLE_UNIX_PATH];
};
#endif
