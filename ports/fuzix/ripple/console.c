/* SPDX-License-Identifier: GPL-2.0-only */
#include "platform.h"
#include <kdata.h>
#include <tty.h>
#include <vm_devices.h>

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
