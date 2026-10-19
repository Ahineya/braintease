/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef FUSE_X_CONFIG_H
#define FUSE_X_CONFIG_H
/* Keep the on-disk C16 layout shared with the Ripple port. Platform services
 * below are hosted implementations; no Ripple machine code is linked. */
#define CONFIG_FUSE_X
#define CONFIG_RIPPLE_SCHED
#define CONFIG_RIPPLE_IO
#define RIPPLE_SCHED_QUANTUM_MS 5U
#include "memory-layout.h"
#define NBUFS 32
#define CONFIG_DYNAMIC_BUFPOOL
#include "../../ports/fuzix/ripple/config-common.h"
#include "../../ports/fuzix/ripple/process-limits.h"
#define PTABSIZE RIPPLE_PROCESS_SLOTS
#define UFTSIZE 32
#define OFTSIZE 192
#define ITABSIZE 128
#define CONFIG_PLATFORM_BRK
/* Native pointer arithmetic requires the canonical next-bank address at an
 * exact bank boundary, rather than the invalid offset 64000 in bank 31. */
#define PROGTOP 0x00200000UL
#undef CMDLINE
#define CMDLINE "0 ro"
/* Fail a bad root mount instead of entering the interactive boot prompt. */
#define BOOTDEVICE 0
extern char *fuse_boot_parameters;
#define BOOTPARAM fuse_boot_parameters
#endif
