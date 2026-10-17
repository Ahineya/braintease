/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef RIPPLE_SOCKET_ABI_H
#define RIPPLE_SOCKET_ABI_H
#include <stdint.h>
/* Supported getsockopt subset, shared by the guest libc and socket bridge. */
#define SOL_SOCKET 1
#define SO_ERROR 4
#define AF_UNIX 2
#define AF_LOCAL AF_UNIX
#define RIPPLE_UNIX_PATH 108
struct ripple_sockaddr_un { uint16_t family; char path[RIPPLE_UNIX_PATH]; };
#endif
