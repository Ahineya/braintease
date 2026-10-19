/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef RIPPLE_FUZIX_BF_CONFIG_H
#define RIPPLE_FUZIX_BF_CONFIG_H
/* Physical memory profiles; these are not the protected RVM port. */
#include "exec-args.h"
#define CONFIG_RIPPLE
#define CONFIG_32BIT
#define CONFIG_SMALL
#define CONFIG_MULTI
#define CONFIG_SINGLETASK
#ifdef RBF_SHELL
#define CONFIG_CUSTOM_VALADDR
#define RBF_RAM_BANKS 8
#define RBF_INIT_CODE 10
#define RBF_APP_CODE 12
#define RBF_IMAGE_BANKS 2
#define PTABSIZE 2
#define BOOTPARAM "rw"
#define RBF_DISK_BLOCKS 1024
#else
#define CONFIG_LEVEL_0
#define RBF_RAM_BANKS 4
#define PTABSIZE 1
#define RBF_DISK_BLOCKS 128
#endif
#define CONFIG_BANKS 1
#define CONFIG_STACKSIZE 2048
#define UFTSIZE 8
#define OFTSIZE 8
#define ITABSIZE 8
#define NBUFS 4
#define NMOUNTS 1
#define NUM_DEV_TTY 1
#define BOOT_TTY 513
#define TTYDEV BOOT_TTY
#define TICKSPERSEC 100
#define BOOTDEVICE 0
#define CMDLINE ""
#define PROGBASE 0x00030000UL
#define PROGLOAD PROGBASE
#define PROGTOP (PROGBASE + BANK_SIZE)
#define plt_copyright() ((void)0)
/* Use the maintained C16 V3 filesystem, on a small raw disk. */
#define BLKSIZE 1024
#define INO_PER_BLOCK 16
#define FS_DIRECT 17
#define FS_LEVELS 3
#define BLKSHIFT 10
#define BLOCK(x) ((x) >> BLKSHIFT)
#define BLKMASK 1023
#define BLKOFF(x) ((uint16_t)(x) & BLKMASK)
#define BLK_TO_OFFSET(x) ((uint32_t)(x) << BLKSHIFT)
#define SMOUNTED 0x7254
#define FILENAME_LEN 30
#define DIR_LEN 32
#endif
