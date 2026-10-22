#ifndef RIPLIX_X86_KERNEL_H
#define RIPLIX_X86_KERNEL_H
#include "bootinfo.h"
#include "io.h"
extern BootInfo boot;
void *memset(void *dest, int value, size_t count);
void *memcpy(void *dest, const void *src, size_t count);
void *memmove(void *dest, const void *src, size_t count);
uint64_t page_alloc(void);
uint64_t pages_alloc(unsigned count);
uint64_t page_available(void);
/* Kernel-owned device/framebuffer page tables, never releasable by contexts. */
uint64_t page_mapping_owned(void);
void page_free(uint64_t address);
void *device_map(uint64_t address, size_t bytes);
void *framebuffer_map(uint64_t address, size_t bytes);
void console_init(void);
void console_flush(void);
void console_graphics(unsigned active);
void console_report(void);
int console_dimensions(unsigned *rows, unsigned *columns);
void console_write(char value);
void console_boot_status(unsigned status, const char *message, const char *detail);
void input_init(void);
void input_poll(void);
int console_input(void);
void screen_status(const char *message);
void runtime_run(void);
void wifi_probe(void);
void timer_init(void);
uint64_t timer_ticks(void);
void timer_idle(uint64_t milliseconds);
#endif
