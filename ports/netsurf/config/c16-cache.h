/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef NETSURF_C16_CACHE_H
#define NETSURF_C16_CACHE_H
#include <stdbool.h>
#include "content/backing_store.h"
#define C16_CACHE_LIMIT 8388608UL /* 16 MiB physical on the C16 filesystem. */
extern struct gui_llcache_table c16_cache_table;
bool c16_cache_enabled(void);
const char *c16_cache_default_path(void);
nserror c16_cache_clear(void);
#endif
