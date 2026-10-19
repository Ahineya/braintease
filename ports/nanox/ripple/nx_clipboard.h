/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef NXR_CLIPBOARD_H
#define NXR_CLIPBOARD_H
#include <stddef.h>
#include "nano-X.h"
#define NXR_CLIPBOARD_MAX 32768UL
/* Private virtual keys; no printable letter or Ctrl+C is synthesized. */
#define NXR_KEY_COPY 0xF8E1
#define NXR_KEY_PASTE 0xF8E2
#define NXR_KEY_CUT 0xF8E3
#define NXR_KEY_SELECT_ALL 0xF8E4
/* A real window owned by this connection is required. Get returns an owned,
 * NUL-terminated UTF-8 snapshot; length excludes NUL. Free it after use.
 * Set is atomic, including across the bounded transport chunks. */
char *nx_clipboard_get(GR_WINDOW_ID window, size_t *length);
int nx_clipboard_set(GR_WINDOW_ID window, const char *text, size_t length);
#endif
