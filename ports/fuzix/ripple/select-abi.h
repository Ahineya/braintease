/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef RIPPLE_SELECT_ABI_H
#define RIPPLE_SELECT_ABI_H
#include <stdint.h>
#include "file-limits.h"
/* Syscall 72: nfds, pointer to this versioned request. All addresses are guest
 * pointers, all sizes are C16 cells. No host fd_set or timeval crosses here. */
#define RIPPLE_SELECT_VERSION 1
#define RIPPLE_SELECT_WORDS 2
struct ripple_fd_set { uint16_t bits[RIPPLE_SELECT_WORDS]; };
/* V1 and its exact two-word memory access remain valid for old executables.
 * V2 keeps the request shape but copies the larger, explicitly versioned set. */
#define RIPPLE_SELECT_VERSION_WIDE 2
#define RIPPLE_SELECT_WIDE_WORDS ((RIPPLE_FILE_LIMIT + 15) / 16)
struct ripple_fd_set_wide { uint16_t bits[RIPPLE_SELECT_WIDE_WORDS]; };
struct ripple_select_time { uint32_t seconds, microseconds; };
struct ripple_select_request {
    uint16_t version;
    void *sets[3];
    const void *timeout;
};
#endif
