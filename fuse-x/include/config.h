/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef FUSE_X_CONFIG_H
#define FUSE_X_CONFIG_H
/* Keep the on-disk C16 layout shared with the Ripple port. Platform services
 * below are hosted implementations; no Ripple machine code is linked. */
#define CONFIG_FUSE_X
#define RIPPLE_PTY_COUNT 4
#include "../../ports/fuzix/ripple/config-common.h"
#define PTABSIZE 16
#define UFTSIZE 16
#define OFTSIZE 64
#define ITABSIZE 64
#define CONFIG_PLATFORM_BRK
/* Native pointer arithmetic requires the canonical next-bank address at an
 * exact bank boundary, rather than the invalid offset 64000 in bank 31. */
#define PROGTOP 0x00200000UL
#undef NUM_DEV_TTY
#define NUM_DEV_TTY 5
#undef CMDLINE
#define CMDLINE "0 ro"
/* Fail a bad root mount instead of entering the interactive boot prompt. */
#define BOOTDEVICE 0
extern char *fuse_boot_parameters;
#define BOOTPARAM fuse_boot_parameters
#endif
