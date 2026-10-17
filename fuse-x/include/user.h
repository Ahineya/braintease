/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef FUSE_X_USER_H
#define FUSE_X_USER_H
#include <kernel.h>
#include <string.h>
#include <tty.h>
extern int fx_errno;
extern long fuse_syscall(unsigned, unsigned long, unsigned long, unsigned long, int *);
long fx_call(unsigned, unsigned long, unsigned long, unsigned long);
int fx_open(const char *, unsigned, unsigned);
int fx_close(int);
long fx_read(int, void *, unsigned);
long fx_write(int, const void *, unsigned);
int fx_fork(void);
int fx_wait(int, int *);
int fx_exec(const char *, char **);
void fx_exit(int);
void fx_shutdown(void);
void fx_puts(const char *);
void fx_number(unsigned);
void fx_error(const char *);
int fx_run(const char *, char **);
struct fx_arguments { unsigned argc; char **argv, **envp; };
#endif
