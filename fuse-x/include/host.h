/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef FUSE_X_HOST_H
#define FUSE_X_HOST_H
#include <kernel.h>
typedef struct HostFile HostFile;
extern HostFile *fopen(const char *, const char *);
extern int fclose(HostFile *);
extern int fseek(HostFile *, long, int);
extern long ftell(HostFile *);
extern size_t fread(void *, size_t, size_t, HostFile *);
extern size_t fwrite(const void *, size_t, size_t, HostFile *);
extern int fflush(HostFile *);
extern void exit(int);
extern void putchar(int);
extern void fuse_disk_open(const char *, unsigned);
extern void fuse_disk_close(void);
extern void fuse_boot_check(void);
extern unsigned fuse_write_check;
extern const char *fuse_value;
extern unsigned fuse_running;
extern unsigned fuse_headless, fuse_telnet, fuse_one_shot, fuse_stopping, fuse_console_closed;
extern void fuse_request_shutdown(void);
extern unsigned fuse_stop_tasks(unsigned);
extern void fuse_shutdown(void);
extern void fuse_poll_input(void);
extern void ripple_network_init(void);
extern void ripple_network_poll(void);
extern arg_t fuse_syscall(unsigned, unsigned long, unsigned long, unsigned long, int *);
#endif
