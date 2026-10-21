/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef FUSE_X_MEMORY_LAYOUT_H
#define FUSE_X_MEMORY_LAYOUT_H
/* Existing binaries keep their 8..30 / 48..239 heaps. New hosted SDK heaps use
 * sparse private banks 256..32767. Kernel globals remain at 240; shared kernel
 * storage occupies 240..255 and 32768..65535. All endpoints are exclusive. */
#define FUSE_LEGACY_HEAP_FIRST_BANK 48
#define FUSE_LEGACY_HEAP_END_BANK 240
#define FUSE_LEGACY_HEAP_BASE (48UL << 16)
/* The current freestanding Context is limited to 256 banks. Select this
 * profile in BOTH its kernel and SDK builds until high-bank backing exists. */
#ifdef FUSE_X_BOUNDED_HEAP
#define FUSE_HEAP_FIRST_BANK 48
#define FUSE_HEAP_END_BANK 240UL
#define FUSE_HEAP_BASE (48UL << 16)
#define FUSE_HEAP_CELLS (192UL * 64000UL)
#else
#define FUSE_HEAP_FIRST_BANK 256
#define FUSE_HEAP_END_BANK 32768UL
#define FUSE_HEAP_BASE (256UL << 16)
#define FUSE_HEAP_CELLS (32512UL * 64000UL)
#endif
#endif
