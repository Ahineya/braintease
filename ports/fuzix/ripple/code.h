/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef RIPPLE_FUZIX_CODE_H
#define RIPPLE_FUZIX_CODE_H
/* Exclusive physical code-bank ceiling; banks 0..31 belong to BIOS/kernel. */
#ifndef RIPPLE_CODE_BANKS
#define RIPPLE_CODE_BANKS 1024U
#endif
#define RIPPLE_CODE_FIRST 32U
#if RIPPLE_CODE_BANKS <= RIPPLE_CODE_FIRST || RIPPLE_CODE_BANKS > 65535UL
#error Invalid Ripple code-bank pool
#endif
/* One MMU leaf covers 256 virtual instruction banks. */
#define RIPPLE_CODE_LEAVES ((RIPPLE_CODE_BANKS - RIPPLE_CODE_FIRST + 255U) / 256U)
#define RIPPLE_CODE_MAP_CELLS (RIPPLE_CODE_LEAVES * 1024U)
#endif
