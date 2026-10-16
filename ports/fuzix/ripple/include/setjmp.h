/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef RIPPLE_SETJMP_H
#define RIPPLE_SETJMP_H
/* RCC callee state: SP, FP, SB, RA, RAB, S0..S3, GP. */
typedef unsigned jmp_buf[10];
int setjmp(jmp_buf env);
void longjmp(jmp_buf env, int value);
#endif
