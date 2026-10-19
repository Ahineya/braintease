/* SPDX-License-Identifier: GPL-2.0-only */
#include <kernel.h>
#include <kdata.h>
/* Avoid rooting the complete upstream syscall table. The shell profile has
 * a small explicit dispatcher; the boot-check profile needs no syscalls. */
struct u_data udata;
ptptr init_process, ptab_end, alarms;
char *cmdline = CMDLINE;
uint16_t ramsize, procmem, maxproc, nproc, runticks, root_dev, waitno;
uint8_t nready, inswap, ticks_this_dsecond, ticks_per_dsecond;
inoptr root;
time_t tod;
ticks_t ticks;
int16_t acct_fh = -1;
struct runload loadavg[3] = {{235, 0}, {251, 0}, {254, 0}};
struct blkbuf bufpool[NBUFS];
struct p_tab ptab[PTABSIZE];
struct oft of_tab[OFTSIZE];
struct cinode i_tab[ITABSIZE];
struct mount fs_tab[NMOUNTS];
