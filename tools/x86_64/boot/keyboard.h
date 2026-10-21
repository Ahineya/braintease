/* US PC keyboard, i8042 translated scan set 1 -> bounded terminal octets.
 * No port I/O, allocator, clock, IRQ or terminal-mode dependency. */
#ifndef RIPLIX_X86_KEYBOARD_H
#define RIPLIX_X86_KEYBOARD_H
#include <stdint.h>

#define X64_KEYBOARD_QUEUE_CAPACITY 64
typedef struct {
    uint8_t output[X64_KEYBOARD_QUEUE_CAPACITY];
    unsigned head, count;
    unsigned modifiers, locks_down, caps, num;
    unsigned extended, pause;
    uint64_t dropped;
} X64Keyboard;

void x64_keyboard_init(X64Keyboard *keyboard);
/* Forget incomplete prefixes and held modifiers after a controller error.
 * Already queued octets and the software Caps/Num lock choices survive. */
void x64_keyboard_resync(X64Keyboard *keyboard);
/* Each key's complete terminal sequence is queued atomically or dropped.
 * Release/modifier state still advances if the output queue is full. */
void x64_keyboard_feed(X64Keyboard *keyboard, uint8_t scan);
/* Returns one terminal octet (including NUL), or -1 when empty. */
int x64_keyboard_get(X64Keyboard *keyboard);
#endif
