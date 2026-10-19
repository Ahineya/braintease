/* SPDX-License-Identifier: GPL-2.0-only */
#include "platform.h"
#include <kdata.h>
#include <tty.h>
#include <vm_devices.h>
#include <string.h>

/* Raw/cbreak curses output can bypass per-character user copies and console
 * callbacks. The caller has already checked foreground group, STOP and DEAD.
 * Copy to kernel RAM: DEVICE_* cannot dereference an MMU user address. */
int ripple_tty_write_run(uint_fast8_t minor) {
    unsigned char buffer[256];
    unsigned n, done, status, i;
    irqflags_t flags;
    tcflag_t oflag = ttydata[minor].termios.c_oflag;
    extern int ripple_pty_write_run(unsigned,const uint8_t *,unsigned,unsigned);
    /* Clamp before narrowing: size_t spans banks, unsigned is one C16 cell. */
    n = udata.u_count - udata.u_done > sizeof(buffer) ? sizeof(buffer) :
        (unsigned)(udata.u_count - udata.u_done);
    if (minor>1 && !(oflag&OPOST))
        return ripple_pty_write_run(minor,udata.u_base,n,udata.u_sysio);
    if (udata.u_sysio) memcpy(buffer, udata.u_base, n);
    else if (uget(udata.u_base, buffer, n)) return -1;
    /* Leave transformed characters to the existing scalar line discipline. */
    if (oflag & OPOST) {
        for (i = 0; i < n; ++i)
            if ((buffer[i] == '\n' && (oflag & ONLCR)) ||
                (buffer[i] == '\r' && (oflag & OCRNL))) break;
        n = i;
        if (!n) return 0;
    }
    if (minor>1) return ripple_pty_write_run(minor,buffer,n,1);
    flags = di();
    status = vm_tty_write(buffer, n, &done);
    irqrestore(flags);
    if (status == VM_DEVICE_UNKNOWN_COMMAND || status == VM_DEVICE_UNSUPPORTED) {
        for (i = 0; i < n; ++i) tty_putc(minor, buffer[i]);
        return n;
    }
    if (status != VM_DEVICE_OK && !done) { udata.u_error = EIO; return -1; }
    return done;
}

/* Only the physical console follows its host. PTY sizes remain controlled by
 * graphics/Telnet clients through TIOCSWINSZ, including their SIGWINCH delivery. */
int ripple_console_ioctl(uint_fast8_t minor, uarg_t request, char *data) {
    if (minor == 1 && request == TIOCGWINSZ) {
        VmTerminalSize size;
        irqflags_t flags = di();
        if (vm_host_terminal_size(&size) == VM_DEVICE_OK && size.rows && size.columns) {
            struct winsize *old = &ttydata[1].winsize;
            if (old->ws_row != size.rows || old->ws_col != size.columns) {
                old->ws_row = size.rows; old->ws_col = size.columns;
                old->ws_xpixel = old->ws_ypixel = 0;
                sgrpsig(ttydata[1].pgrp, SIGWINCH);
            }
        }
        irqrestore(flags);
    }
    return tty_ioctl(minor, request, data);
}
