/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef RIPPLE_FUZIX_CONFIG_H
#define RIPPLE_FUZIX_CONFIG_H
#include "mmu-profile.h"
#define NBUFS 16
#define CONFIG_DYNAMIC_BUFPOOL
#include "config-common.h"
#include "process-limits.h"
#define PTABSIZE RIPPLE_PROCESS_SLOTS
#include "file-limits.h"
#define UFTSIZE RIPPLE_FILE_LIMIT
#define OFTSIZE RIPPLE_OPEN_FILE_LIMIT
#define CONFIG_DYNAMIC_OFT
#define ITABSIZE 128
#define CONFIG_RIPPLE_SCHED
#define CONFIG_RIPPLE_IO
#define CONFIG_PLATFORM_BRK
#define PROGTOP (0x001f0000UL + 64000UL)
#endif
