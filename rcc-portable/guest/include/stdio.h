/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef RCC_PORTABLE_GUEST_STDIO_H
#define RCC_PORTABLE_GUEST_STDIO_H
#include "../../../ports/fuzix/ripple/include/stdio.h"
/* Compiler-local fast paths for the Ripple SDK's FILE ABI. Only buffered
 * characters bypass libc: refill, pushback, mode changes and flushing still
 * use the original functions. As permitted for getc/putc, the stream argument
 * may be evaluated more than once; the character argument is evaluated once.
 * The flags are READING=1, WRITING=2, ACTIVE_WRITE=4 in ripple/libc/stdio.c. */
#undef getc
#undef putc
#define getc(f)                                                                                     \
    ((f)->used && ((f)->flags & 5) == 1 && (f)->pushed == EOF && (f)->pos < (f)->len                    \
         ? (int)(unsigned char)(f)->buf[(f)->pos++]                                                  \
         : fgetc(f))
#define putc(c, f)                                                                                  \
    ((f)->used && ((f)->flags & 6) == 6 && (f)->buffering == _IOFBF && (f)->len < BUFSIZ - 1            \
         ? (int)(unsigned char)((f)->buf[(f)->len++] = (c))                                          \
         : fputc((c), (f)))
#endif
