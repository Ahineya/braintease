/* SPDX-License-Identifier: GPL-2.0-only */
#include <kernel.h>
#include <version.h>
struct sysinfoblk sysinfo = {sizeof(struct sysinfoblk),
                             CONFIG_BANKS,
                             UFTSIZE,
                             PTABSIZE,
                             TICKSPERSEC,
                             0,
                             0,
                             0,
                             {0, 0, 0},
                             0,
                             0,
                             0,
                             {0, 0, 0},
                             {0, 0, 0, 0, 0, 0, 0, 0},
                             "0.5\0Fuzix\0ripple\0Ripple"};
