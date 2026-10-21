#ifndef RIPLIX_X86_IO_H
#define RIPLIX_X86_IO_H
#include <stdint.h>
void boot_diagnostic(const char *message, uint64_t status);

static inline void out8(uint16_t port, uint8_t value) {
    __asm__ volatile("outb %0, %1" : : "a"(value), "Nd"(port));
}
static inline uint8_t in8(uint16_t port) {
    uint8_t value;
    __asm__ volatile("inb %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}
static inline void out32(uint16_t port, uint32_t value) {
    __asm__ volatile("outl %0, %1" : : "a"(value), "Nd"(port));
}
static inline uint32_t in32(uint16_t port) {
    uint32_t value;
    __asm__ volatile("inl %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}
static inline uint64_t rdtsc(void) {
    uint32_t low, high;
    __asm__ volatile("lfence; rdtsc" : "=a"(low), "=d"(high) : : "memory");
    return ((uint64_t)high << 32) | low;
}
static inline void serial_init(void) {
    out8(0x3f9, 0); out8(0x3fb, 0x80);
    out8(0x3f8, 1); out8(0x3f9, 0);
    out8(0x3fb, 3); out8(0x3fa, 0xc7); out8(0x3fc, 0x0b);
}
static inline void serial_char(char c) {
    for (unsigned wait = 0; wait < 100000; ++wait) {
        uint8_t status = in8(0x3fd);
        if (status == 0xff) return;
        if (status & 0x20) { out8(0x3f8, (uint8_t)c); return; }
    }
}
static inline void serial(const char *s) {
    while (*s) { if (*s == '\n') serial_char('\r'); serial_char(*s++); }
}
static inline void hex(uint64_t value) {
    serial("0x");
    for (int shift = 60; shift >= 0; shift -= 4)
        serial_char("0123456789abcdef"[(value >> shift) & 15]);
}
static inline _Noreturn void stop(unsigned code) {
#ifdef BOOT_TEST
    __asm__ volatile("outl %0, %1" : : "a"(code), "Nd"((uint16_t)0xf4));
#else
    (void)code;
#endif
    for (;;) __asm__ volatile("cli; hlt");
}
static inline _Noreturn void fail(const char *message, uint64_t status) {
    boot_diagnostic(message, status);
    serial("BOOT_FAIL: "); serial(message); serial(" "); hex(status); serial("\n");
    stop(0x11);
}
#endif
