#ifndef MMIO_H
#define MMIO_H

// Memory-Mapped I/O Interface for Ripple VM
// Provides access to hardware devices at fixed memory addresses

#include <mmio_constants.h>

// MMIO access functions (implemented in mmio.c)
unsigned short mmio_read(unsigned short addr);
void mmio_write(unsigned short addr, unsigned short value);

// TTY functions
void tty_putchar(unsigned char c);

// RNG functions
unsigned short rng_get(void);
unsigned short rng_get_seed(void);
void rng_set_seed(unsigned short seed);

// Display functions
void display_set_mode(unsigned short mode);
void display_enable(void);
void display_clear(void);
void display_flush(void);

// TEXT40 VRAM access
void text40_putchar(int x, int y, unsigned char c);
void text40_puts(int x, int y, const char* s);

// TEXT40 colored character functions
void text40_putchar_color(int x, int y, unsigned char c, unsigned char fg, unsigned char bg);
void text40_putchar_attr(int x, int y, unsigned char c, unsigned char attr);
void text40_puts_color(int x, int y, const char* s, unsigned char fg, unsigned char bg);
void text40_puts_attr(int x, int y, const char* s, unsigned char attr);

// TEXT40 attribute functions
void text40_set_attr(int x, int y, unsigned char attr);
unsigned char text40_get_char(int x, int y);
unsigned char text40_get_attr(int x, int y);

// Keyboard input functions (TEXT40 mode only)
int key_pressed(unsigned short key_addr);
int key_up_pressed(void);
int key_down_pressed(void);
int key_left_pressed(void);
int key_right_pressed(void);
int key_z_pressed(void);
int key_x_pressed(void);

// Storage functions
void storage_set_block(unsigned short block);
void storage_set_addr(unsigned short addr);
unsigned short storage_read(void);
void storage_write(unsigned short value);
unsigned short storage_get_status(void);
void storage_commit(void);
void storage_commit_all(void);
int storage_is_busy(void);
int storage_is_dirty(void);

// High-level storage operations
void storage_write_at(unsigned short block, unsigned short addr, unsigned short value);
unsigned short storage_read_at(unsigned short block, unsigned short addr);
void storage_write_buffer(unsigned short block, unsigned short addr, const unsigned short* data, unsigned short count);
void storage_read_buffer(unsigned short block, unsigned short addr, unsigned short* data, unsigned short count);

// Interrupt controller
// Do not enable a STATUS bit that is already pending unless a handler is installed:
// the VM dispatches at the start of the next instruction.
unsigned short irq_get_status(void);
void irq_raise(unsigned short bits);
unsigned short irq_get_enable(void);
void irq_set_enable(unsigned short mask);
int irq_is_busy(void);
void irq_ack(void);
void irq_set_vector(unsigned short bank, unsigned short off);
unsigned short irq_get_cause(void);

#endif // MMIO_H