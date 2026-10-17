/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef RIPPLE_PTY_H
#define RIPPLE_PTY_H
void ripple_pty_init(void);
unsigned ripple_pty_status(unsigned);
int ripple_slave_open(uint_fast8_t, uint16_t);
int ripple_slave_close(uint_fast8_t);
void ripple_pty_putc(unsigned, unsigned);
int ripple_pty_ready(unsigned);
int ripple_pty_carrier(unsigned);
int ripple_pty_open(uint_fast8_t, uint16_t);
int ripple_pty_close(uint_fast8_t);
int ripple_pty_read(uint_fast8_t, uint_fast8_t, uint_fast8_t);
int ripple_pty_write(uint_fast8_t, uint_fast8_t, uint_fast8_t);
int ripple_pty_ioctl(uint_fast8_t, uarg_t, char *);
#endif
