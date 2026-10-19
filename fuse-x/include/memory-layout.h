/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef FUSE_X_MEMORY_LAYOUT_H
#define FUSE_X_MEMORY_LAYOUT_H
/* Keep the heap disjoint from frames (2..4), arguments (31), application
 * globals (32..47), and the shared kernel arena (240..255). */
#define FUSE_HEAP_FIRST_BANK 48
#define FUSE_HEAP_END_BANK 240
#define FUSE_HEAP_BASE (48UL << 16)
#define FUSE_HEAP_CELLS (192UL * 64000UL)
#endif
