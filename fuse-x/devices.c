/* SPDX-License-Identifier: GPL-2.0-only */
#include "host.h"
#include <kdata.h>
#include <printf.h>
#include <tty.h>
#include <native_hosted.h>
#include "pty.h"
#include "graphics.h"

static HostFile *disk;
static unsigned writable;
static unsigned long disk_size;
static unsigned char octets[2 * BLKSIZE];
static uint8_t ttybuf[256];
static unsigned input_eof;
extern int fuse_system_open(uint_fast8_t,uint16_t);
extern int fuse_system_read(uint_fast8_t,uint_fast8_t,uint_fast8_t);
extern int fuse_system_write(uint_fast8_t,uint_fast8_t,uint_fast8_t);
extern int fuse_system_ioctl(uint_fast8_t,uarg_t,char *);
struct s_queue ttyinq[NUM_DEV_TTY + 1];
tcflag_t termios_mask[NUM_DEV_TTY + 1] = {0, 65535U};

void fuse_disk_open(const char *path, unsigned write) {
    if (sizeof(struct dinode) != 64 || sizeof(struct filesys) != 164) panic("Fuse-X: C16 V3 layout mismatch");
    const char *signature = "RIPPLE-FUZIX-C16-V3";
    unsigned i;
    if(rcc_hosted_lock_image(path,write)) panic("Fuse-X: cannot open root image or image is already in use");
    disk = fopen(path, write ? "r+b" : "rb");
    if (!disk) panic("Fuse-X: cannot open root image");
    writable = write;
    if (fseek(disk, 0L, 2) || (long)(disk_size = ftell(disk)) < 0 ||
        disk_size < 8UL * BLKSIZE || disk_size % (2UL * BLKSIZE) ||
        disk_size / (2UL * BLKSIZE) > 2097151UL)
        panic("Fuse-X: invalid root image size");
    if (fseek(disk, 0L, 0) || fread(octets, 1, 19, disk) != 19)
        panic("Fuse-X: root image header read failed");
    for (i = 0; i < 19; i++)
        if (octets[i] != signature[i]) panic("Fuse-X: expected a raw C16 V3 root image");
}
void fuse_disk_close(void) {
    if (fflush(disk) || fclose(disk)) panic("Fuse-X: root image close failed");
    if(writable && rcc_hosted_sync_image()) panic("Fuse-X: root image sync failed");
    disk = NULL;
}
static int root_open(uint_fast8_t minor, uint16_t flags) { used(flags); return minor ? -1 : 0; }
static int root_io(uint_fast8_t minor, uint_fast8_t raw, uint_fast8_t flags, unsigned write) {
    unsigned i;
    unsigned long offset = (unsigned long)udata.u_block * (2UL * BLKSIZE);
    used(flags);
    if (minor || raw) { udata.u_error = ENXIO; return -1; }
    if (write && !writable) { udata.u_error = EROFS; return -1; }
    if (offset >= disk_size || disk_size - offset < 2UL * BLKSIZE || fseek(disk, offset, 0)) {
        udata.u_error = EIO; return -1;
    }
    if (write) {
        for (i = 0; i < BLKSIZE; i++) {
            octets[2 * i] = udata.u_dptr[i] & 255;
            octets[2 * i + 1] = udata.u_dptr[i] >> 8;
        }
        if (fwrite(octets, 1, 2 * BLKSIZE, disk) != 2 * BLKSIZE) { udata.u_error = EIO; return -1; }
    } else {
        if (fread(octets, 1, 2 * BLKSIZE, disk) != 2 * BLKSIZE) { udata.u_error = EIO; return -1; }
        for (i = 0; i < BLKSIZE; i++) udata.u_dptr[i] = octets[2 * i] | (octets[2 * i + 1] << 8);
    }
    return BLKSIZE;
}
static int root_read(uint_fast8_t m, uint_fast8_t r, uint_fast8_t f) { return root_io(m, r, f, 0); }
static int root_write(uint_fast8_t m, uint_fast8_t r, uint_fast8_t f) { return root_io(m, r, f, 1); }
static int root_ioctl(uint_fast8_t minor, uarg_t request, char *p) {
    used(minor); used(p);
    if (request == BLKFLSBUF) {
        if (!fflush(disk)) return 0;
        udata.u_error = EIO;
    } else udata.u_error = ENOTTY;
    return -1;
}
void kputchar(uint_fast8_t c) { if (c == '\n') putchar('\r'); putchar(c & 255); }
/* The guest line discipline already applies terminal output processing. */
void tty_putc(uint_fast8_t minor, uint_fast8_t c) {
    if (minor > 1) ripple_pty_putc(minor,c);
    else putchar(c & 255);
}
void tty_setup(uint_fast8_t minor, uint_fast8_t flags) { used(minor); used(flags); }
int tty_carrier(uint_fast8_t minor) { return minor == 1 || ripple_pty_carrier(minor); }
void tty_sleeping(uint_fast8_t minor) { used(minor); }
void tty_data_consumed(uint_fast8_t m) { if(m>1){ripple_gfx_wakeup();selwake_dev(3,m-2,SELECT_OUT);} }
ttyready_t tty_writeready(uint_fast8_t minor) {
    return minor == 1 || ripple_pty_ready(minor) ? TTY_READY_NOW : TTY_READY_LATER;
}
void fuse_poll_input(void) {
    int c;
    unsigned count = 0;
    if (!fuse_running) return;
    if (input_eof) {
        if (!ttyinq[1].q_count) tty_inproc(1,CTRL('D'));
        return;
    }
    /* Retain queue backpressure so piped commands are not silently dropped. */
    while (count++ < 32) {
        if (ttyinq[1].q_count >= ttyinq[1].q_size - 1) {
            /* Raw/cbreak input has no line terminator to wait for. Leave the
             * host backlog untouched until the reader frees queue space. */
            if (!(ttydata[1].termios.c_lflag & ICANON)) return;
            unsigned i;
            uint8_t *p=ttyinq[1].q_head;
            for (i=0;i<ttyinq[1].q_count;i++) {
                if (*p=='\n' || *p==CTRL('D')) return;
                if (++p==ttyinq[1].q_base+ttyinq[1].q_size) p=ttyinq[1].q_base;
            }
            /* An unfinished overlong line still needs its terminator. The
             * canonical line discipline reserves a slot and drops its tail. */
        }
        c = rcc_hosted_input();
        if (c == -1) break;
        if (c == -2) { input_eof = fuse_console_closed = 1; tty_inproc(1, CTRL('D')); break; }
        tty_inproc(1, c);
    }
}
void device_init(void) {
    struct s_queue *q = &ttyinq[1];
    q->q_base = q->q_head = q->q_tail = ttybuf;
    q->q_size = 256;
    q->q_wakeup = 128;
    ttydata[1].termios.c_cc[VERASE] = 127; /* macOS terminal Delete key */
    ripple_pty_init();
    ripple_network_init();
}
extern int ripple_console_ioctl(uint_fast8_t, uarg_t, char *);
struct devsw dev_tab[] = {
    {root_open, no_close, root_read, root_write, root_ioctl},
    {fuse_system_open, no_close, fuse_system_read, fuse_system_write, fuse_system_ioctl},
    {ripple_slave_open, ripple_slave_close, tty_read, tty_write, ripple_console_ioctl},
    {ripple_pty_open, ripple_pty_close, ripple_pty_read, ripple_pty_write, ripple_pty_ioctl},
    {ripple_gfx_open, ripple_gfx_close, no_rdwr, no_rdwr, ripple_gfx_ioctl}
};
bool validdev(uint16_t dev) { return major(dev) < 5; }
