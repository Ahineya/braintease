/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef RIPPLE_FUZIX_USER_H
#define RIPPLE_FUZIX_USER_H
#include <stdint.h>
#include <stddef.h>
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
extern int error;
long call(unsigned, long, long, long, long);
int open(const char *, unsigned, unsigned);
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
