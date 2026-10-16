/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef RIPPLE_FUZIX_RESOURCES_H
#define RIPPLE_FUZIX_RESOURCES_H
#include <stdint.h>

/* Read-only snapshot from /dev/proc. Counts use physical KiB (1024 octets),
 * not C characters. Shared pages are counted once in used_kib; mapped_kib
 * counts each process mapping. No kernel addresses are exposed. */
#define RIPPLE_SYSINFO 0x5201
#define RIPPLE_SYSINFO_VERSION 1
#define RIPPLE_SYSINFO_MMU 1
struct ripple_sysinfo {
    uint16_t version, flags, page_cells, processes, slots, ticks_per_second;
    uint32_t ram_kib, pool_kib, used_kib, shared_kib, mapped_kib;
    uint32_t heap_cells, stack_cells, uptime_ticks;
};
/* Input: slot (0..slots-1). Output: one coherent, address-free process record.
 * CPU counters and sample_ticks use cpu_hz, unlike SYSINFO deciseconds.
 * mapped = private + shared. Shared is per-process and must not be summed. */
#define RIPPLE_PROCINFO 0x5202
#define RIPPLE_PROCINFO_VERSION 1
struct ripple_procinfo {
    uint16_t slot, version, pid, ppid, pgrp, session, uid, tty;
    uint16_t state, priority, nice, cpu_hz;
    uint32_t start_ticks, user_ticks, system_ticks, sample_ticks;
    uint32_t mapped_kib, private_kib, shared_kib;
    char name[9];
};
#endif
