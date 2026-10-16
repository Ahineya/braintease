/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef RIPPLE_FUZIX_CONFIG_H
#define RIPPLE_FUZIX_CONFIG_H
#define CONFIG_RIPPLE
#define CONFIG_32BIT
#define CONFIG_MULTI
#define CONFIG_PARENT_FIRST
#define CONFIG_CUSTOM_VALADDR
#define CONFIG_SMALL
#define CONFIG_BANKS 1
#define CONFIG_STACKSIZE 2048
#define PTABSIZE 4
#define NBUFS 8
#define NMOUNTS 2
#define NUM_DEV_TTY 1
#define BOOT_TTY 512 + 1
#define TTYDEV (BOOT_TTY)
#define TICKSPERSEC 100
#define CMDLINE "0 rw"
#define BOOTDEVICENAMES "hd#"
#define PROGBASE 0x00080000UL
#define PROGLOAD 0x00080000UL
#define PROGTOP (PROGBASE + 64000UL)
#define plt_copyright() ((void)0)
/* A disk block is 256 C characters / 512 physical octets. */
#define BLKSIZE 256
#define INO_PER_BLOCK 8
#define BLKSHIFT 8
#define BLOCK(x) ((x) >> BLKSHIFT)
#define BLKMASK 255
#define BLKOFF(x) ((uint16_t)(x) & BLKMASK)
#define BLK_TO_OFFSET(x) ((uint32_t)(x) << BLKSHIFT)
#define SMOUNTED 0x7252
#define FILENAME_LEN 30
#define DIR_LEN 32
#endif
