/* SPDX-License-Identifier: GPL-2.0-only */
#include <kernel.h>
#include <version.h>
#include "identity.h"
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
                             RIPLIX_RELEASE "\0" RIPLIX_SYSNAME "\0ripple\0Ripple"};
