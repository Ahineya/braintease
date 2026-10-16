/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef RIPPLE_FUZIX_USER_H
#define RIPPLE_FUZIX_USER_H
#include <stddef.h>
#include <stdint.h>
struct request {
    unsigned call;
    long arg[4];
    int error;
};
struct dirent {
    unsigned ino;
    char name[30];
    unsigned padding;
};
extern long ripple_syscall(struct request *);
extern int ripple_error;
long call(unsigned, long, long, long, long);
int open(const char *, int, ...);
int close(int);
long read(int, void *, unsigned long);
long write(int, const void *, unsigned long);
void out(const char *);
void number(unsigned);
void user_exit(void);
size_t strlen(const char *);
int strcmp(const char *, const char *);
void *memcpy(void *, const void *, size_t);
void *memset(void *, int, size_t);
#endif
