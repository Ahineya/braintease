/* Polled PC console: COM1 plus an i8042 keyboard in translated scan set 1.
 * No IRQs or firmware calls. USB/HID and non-US layouts are later drivers. */
#include "kernel.h"
#include "keyboard.h"
#ifdef BOOT_TOUCHPAD
#include "touchpad.h"
#endif
#ifdef BOOT_GRAPHICS
#include "gx.h"
static void graphics_key(void *opaque, unsigned usage, unsigned down) {
    (void)opaque;
    gx_key(usage,down,0);
}
#endif
static unsigned keyboard_ready;
static X64Keyboard keyboard;
static int writable(void) {
    uint64_t end = rdtsc() + boot.tsc_hz / 20;
    while (in8(0x64) & 2) if (rdtsc() >= end) return 0;
    return 1;
}
static int controller(uint8_t command) {
    if (!writable()) return 0;
    out8(0x64, command); return 1;
}
static int data(uint8_t value) {
    if (!writable()) return 0;
    out8(0x60, value); return 1;
}
static int response(void) {
    uint64_t end = rdtsc() + boot.tsc_hz / 20;
    do {
        uint8_t status = in8(0x64);
        if (status & 1) {
            unsigned value = in8(0x60);
            if (!(status & 0x20)) return value;
        }
    } while (rdtsc() < end);
    return -1;
}
static int command(uint8_t value) {
    for (unsigned retry = 0; retry < 2; ++retry) {
        if (!data(value)) return 0;
        int result = response();
        if (result == 0xfa) return 1;
        if (result != 0xfe) return 0;
    }
    return 0;
}
void input_init(void) {
    keyboard_ready = 0;
    x64_keyboard_init(&keyboard);
#ifdef BOOT_GRAPHICS
    x64_keyboard_events(&keyboard,graphics_key,0);
#endif
    if (!controller(0xad) || !controller(0xa7)) goto unavailable;
    for (unsigned i = 0; i < 32 && (in8(0x64) & 1); ++i) (void)in8(0x60);
    if (!controller(0x20)) goto unavailable;
    int configuration = response();
    if (configuration < 0 || !controller(0x60) || !data((configuration | 0x60) & ~0x13) ||
        !controller(0xae) || !command(0xf5) || !command(0xf0) || !command(2) || !command(0xf4))
        goto unavailable;
    keyboard_ready = 1; serial("PS2_KEYBOARD_OK\n");
    goto devices;
unavailable:
    serial("PS2_KEYBOARD_UNAVAILABLE serial_input_only\n");
devices:;
#ifdef BOOT_TOUCHPAD
    /* PCI/I2C input is independent of an absent or failed PS/2 controller. */
    touchpad_init();
#endif
}
void input_poll(void) {
#ifdef BOOT_TOUCHPAD
    touchpad_poll();
#endif
    for (unsigned i = 0; keyboard_ready && i < 16 && (in8(0x64) & 1); ++i) {
        uint8_t status = in8(0x64), scan = in8(0x60);
        if (status & 0x20) continue;
        if (status & 0xc0) { x64_keyboard_resync(&keyboard); continue; }
        x64_keyboard_feed(&keyboard,scan);
    }
#ifdef BOOT_GRAPHICS
    if (gx_active()) while (x64_keyboard_get(&keyboard) >= 0) {}
#endif
}
int console_input(void) {
    console_flush();
#ifdef BOOT_GRAPHICS
    if (gx_active()) {
        input_poll();
        uint8_t uart=in8(0x3fd);
        return uart!=0xff && (uart&1) ? in8(0x3f8) : -1;
    }
#endif
    /* Finish an already-started ANSI key token before accepting serial input,
     * so a UART byte cannot split a keyboard escape sequence. */
    int pending = x64_keyboard_get(&keyboard);
    if (pending >= 0) return pending;
    /* An absent legacy UART commonly returns all ones. It must not flood the
     * shell with fictitious input on laptops that expose no working COM1. */
    uint8_t uart = in8(0x3fd);
    if (uart != 0xff && (uart & 1)) return in8(0x3f8);
    input_poll();
    return x64_keyboard_get(&keyboard);
}
