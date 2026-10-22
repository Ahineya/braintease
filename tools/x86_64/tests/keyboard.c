/* Production decoder and polled input adapter, with host i8042/UART fixtures.
 * Protocol source versions are pinned in boot/keyboard.c. These tests exercise
 * the translated set-1 path, not raw set 2 or real HP firmware hotkey routing. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "../boot/keyboard.h"

static void feed(X64Keyboard *k, const char *scans) {
    while (*scans) x64_keyboard_feed(k,(uint8_t)*scans++);
}
static void expect_bytes(X64Keyboard *k, const char *bytes, unsigned count) {
    for (unsigned i=0; i<count; ++i) {
        int got=x64_keyboard_get(k);
        if (got!=(uint8_t)bytes[i]) fprintf(stderr,"octet %u: got %d expected %u\n",i,got,(uint8_t)bytes[i]);
        assert(got==(uint8_t)bytes[i]);
    }
    assert(x64_keyboard_get(k)==-1);
}
#define EXPECT(k,s) expect_bytes((k),(s),sizeof(s)-1)

static void characters(void) {
    X64Keyboard k; x64_keyboard_init(&k);
    feed(&k,"\x01\x81\x1e\x9e\x0e\x8e\x0f\x8f\x1c\x9c");
    EXPECT(&k,"\033a\177\t\r");
    feed(&k,"\x2a\x02\x03\x04\x05\x06\x07\x08\x09\x0a\x0b\x0c\x0d\x1a\x1b\x27\x28\x29\x2b\x33\x34\x35\xaa");
    EXPECT(&k,"!@#$%^&*()_+{}:\"~|<>?");
    feed(&k,"\x1e\x1e\x9e"); EXPECT(&k,"aa"); /* Typematic characters repeat. */
    feed(&k,"\x1d\x1e\x2a\x1e\x03\x07\x0c\x35\xaa\x1a\x2b\x1b\x39\x0e\x9d");
    EXPECT(&k,"\001\001\000\036\037\177\033\034\035\000\010");
    feed(&k,"\x38\x2d\xb8\x2d\xe0\x38\x1e\xe0\xb8\x1e");
    EXPECT(&k,"\033xx\033aa");
    feed(&k,"\x2a\x0f\xaa"); EXPECT(&k,"\033[Z");
}
static void modifier_lifetimes(void) {
    X64Keyboard k; x64_keyboard_init(&k);
    feed(&k,"\x2a\x36\xaa\x1e\xb6\x1e"); EXPECT(&k,"Aa");
    feed(&k,"\x1d\xe0\x1d\x9d\x1e\xe0\x9d\x1e"); EXPECT(&k,"\001a");
    feed(&k,"\x38\xe0\x38\xb8\x1e\xe0\xb8\x1e"); EXPECT(&k,"\033aa");
    feed(&k,"\x3a\x3a\x1e\xba\x1e\x2a\x1e\xaa\x3a\xba\x1e");
    EXPECT(&k,"AAaa"); /* Lock repeats do not toggle until released. */
    feed(&k,"\x2a\xe0\x2a\xe0\x37\xe0\xb7\xe0\xaa\x1e\xaa\x1e");
    EXPECT(&k,"Aa"); /* Fake PrintScreen shift must not release a real one. */
    assert(!k.modifiers && !k.extended && !k.pause);
}
static void navigation_and_functions(void) {
    X64Keyboard k; x64_keyboard_init(&k);
    feed(&k,"\xe0\x48\xe0\xc8\xe0\x50\xe0\xd0\xe0\x4b\xe0\xcb\xe0\x4d\xe0\xcd\xe0\x47\xe0\x4f\xe0\x52\xe0\x53\xe0\x49\xe0\x51");
    EXPECT(&k,"\033[A\033[B\033[D\033[C\033[H\033[F\033[2~\033[3~\033[5~\033[6~");
    feed(&k,"\x3b\xbb\x3c\xbc\x3d\xbd\x3e\xbe\x3f\xbf\x40\xc0\x41\xc1\x42\xc2\x43\xc3\x44\xc4\x57\xd7\x58\xd8");
    EXPECT(&k,"\033OP\033OQ\033OR\033OS\033[15~\033[17~\033[18~\033[19~\033[20~\033[21~\033[23~\033[24~");
    feed(&k,"\x2a\xe0\x48\x3b\x58\xaa\x1d\xe0\x4d\x9d\x38\xe0\x53\xb8");
    EXPECT(&k,"\033[1;2A\033[1;2P\033[24;2~\033[1;5C\033[3;3~");
    feed(&k,"\x2a\x1d\x38\xe0\x47\xb8\x9d\xaa"); EXPECT(&k,"\033[1;8H");
}
static void keypad_and_pause(void) {
    X64Keyboard k; x64_keyboard_init(&k);
    feed(&k,"\xe0\x1c\xe0\x35\x37\x4a\x4e\x48\x4c");
    EXPECT(&k,"\r/*-+\033[A\033[E");
    feed(&k,"\x45\x45\xc5\x47\x48\x49\x4b\x4c\x4d\x4f\x50\x51\x52\x53");
    EXPECT(&k,"7894561230.");
    feed(&k,"\x2a\x48\xaa\xe0\x48"); EXPECT(&k,"\033[1;2A\033[A");
    feed(&k,"\x45\xc5\xe1\x1d\x45\xe1\x9d\xc5\x1e"); EXPECT(&k,"a");
    assert(!k.num && !k.modifiers && !k.pause);
    feed(&k,"\x1d\xe1\x1d\x45\xe1\x9d\xc5\x1e\x9d\x1e"); EXPECT(&k,"\001a");
    feed(&k,"\xe1\x1d\x1e\xe0\xe0\x48\xe0\xe1\x1d\x45\xe1\x9d\xc5\x1e");
    EXPECT(&k,"a\033[Aa"); /* Malformed E1 recovers at first non-tail byte. */
    feed(&k,"\xe0\xfa\x1e\xfe\xfc\xfd\xf0\xe0\x5b\xe0\x20\x1e");
    EXPECT(&k,"aa"); /* Protocol responses and unsupported keys are silent. */
}
static void overflow_and_recovery(void) {
    X64Keyboard k; x64_keyboard_init(&k);
    for (unsigned i=0; i<X64_KEYBOARD_QUEUE_CAPACITY-1; ++i) x64_keyboard_feed(&k,0x1e);
    feed(&k,"\xe0\x48"); assert(k.dropped==1 && k.count==X64_KEYBOARD_QUEUE_CAPACITY-1);
    x64_keyboard_feed(&k,0x30); assert(k.count==X64_KEYBOARD_QUEUE_CAPACITY);
    feed(&k,"\x2a\x30\xaa"); assert(k.dropped==2 && !k.modifiers);
    for (unsigned i=0; i<X64_KEYBOARD_QUEUE_CAPACITY-1; ++i) assert(x64_keyboard_get(&k)=='a');
    assert(x64_keyboard_get(&k)=='b'); assert(x64_keyboard_get(&k)==-1);
    feed(&k,"\xe0\x48"); EXPECT(&k,"\033[A"); /* No partial ESC from overflow. */
    for (unsigned i=0; i<X64_KEYBOARD_QUEUE_CAPACITY-1; ++i) x64_keyboard_feed(&k,0x1e);
    feed(&k,"\x38\x1e\xb8\x30");
    assert(k.dropped==3 && !k.modifiers && k.count==X64_KEYBOARD_QUEUE_CAPACITY);
    for (unsigned i=0; i<X64_KEYBOARD_QUEUE_CAPACITY-1; ++i) assert(x64_keyboard_get(&k)=='a');
    assert(x64_keyboard_get(&k)=='b' && x64_keyboard_get(&k)==-1);
    for (unsigned cycle=0; cycle<1000; ++cycle) {
        feed(&k,"\x58\x38\x1e\xb8"); EXPECT(&k,"\033[24~\033a");
    }
    feed(&k,"\x3a\xba\x1e\x2a\x1d\x38\xe1\x1d");
    x64_keyboard_resync(&k); assert(k.caps && !k.modifiers && !k.pause);
    EXPECT(&k,"A"); feed(&k,"\x1e"); EXPECT(&k,"A");
    feed(&k,"\x3a\xba\x2a"); x64_keyboard_feed(&k,0xff); feed(&k,"\x1e"); EXPECT(&k,"a");
    feed(&k,"\x1d"); x64_keyboard_feed(&k,0); feed(&k,"\x1e"); EXPECT(&k,"a");
    k.dropped=UINT64_MAX;
    for (unsigned i=0; i<X64_KEYBOARD_QUEUE_CAPACITY+1; ++i) x64_keyboard_feed(&k,0x1e);
    assert(k.dropped==UINT64_MAX);
}
static void random_streams(void) {
    X64Keyboard k; x64_keyboard_init(&k); uint32_t random=0x40f359a1;
    for (unsigned i=0; i<500000; ++i) {
        random=random*1664525+1013904223;
        x64_keyboard_feed(&k,(uint8_t)(random>>24));
        if (random&1) (void)x64_keyboard_get(&k);
        if (!(random&1023)) x64_keyboard_resync(&k);
        assert(k.count<=X64_KEYBOARD_QUEUE_CAPACITY && k.head<X64_KEYBOARD_QUEUE_CAPACITY);
        assert(k.pause<=5 && k.extended<=1 && k.modifiers<64 && k.locks_down<4);
    }
}
static unsigned events[32], event_count;
static void raw_event(void *opaque,unsigned usage,unsigned down) {
    assert(opaque==events && event_count<32);
    events[event_count++]=usage*2+down;
}
static void graphical_edges(void) {
    X64Keyboard k; x64_keyboard_init(&k);
    x64_keyboard_events(&k,raw_event,events);
    feed(&k,"\x2a\x1e\x9e\xaa\xe0\x48\xe0\xc8\x3b\xbb\xe0\x2a\xe0\xaa");
    const unsigned expected[]={451,9,8,450,165,164,117,116};
    assert(event_count==sizeof(expected)/sizeof(*expected));
    for(unsigned i=0;i<event_count;i++)assert(events[i]==expected[i]);
    EXPECT(&k,"A\033[A\033OP");
    x64_keyboard_resync(&k);
    assert(k.event==raw_event && k.event_opaque==events);
}

/* The production adapter is compiled here with port I/O mocked. */
#include "../boot/bootinfo.h"
#define RIPLIX_X86_KERNEL_H
BootInfo boot;
static struct { uint8_t status, value; } hardware[128];
static unsigned hardware_at, hardware_count, uart_status, uart_value, flushes;
static uint64_t rdtsc(void) { static uint64_t ticks; return ++ticks; }
static uint8_t in8(uint16_t port) {
    if (port==0x3fd) return uart_status;
    if (port==0x3f8) { uart_status=0; return uart_value; }
    if (port==0x64) return hardware_at<hardware_count ? hardware[hardware_at].status|1 : 0;
    if (port==0x60) { assert(hardware_at<hardware_count); return hardware[hardware_at++].value; }
    assert(0); return 0;
}
static void out8(uint16_t port, uint8_t value) { (void)port; (void)value; }
static void serial(const char *value) { (void)value; }
static void console_flush(void) { ++flushes; }
#include "../boot/input.c"
static void enqueue_scan(uint8_t scan, uint8_t status) {
    assert(hardware_count<sizeof(hardware)/sizeof(hardware[0]));
    hardware[hardware_count].value=scan; hardware[hardware_count++].status=status;
}
static void adapter(void) {
    x64_keyboard_init(&keyboard); keyboard_ready=1;
    enqueue_scan(0xe0,0); enqueue_scan(0x48,0);
    assert(console_input()==27);
    uart_status=1; uart_value='s';
    assert(console_input()=='['); assert(console_input()=='A');
    assert(console_input()=='s'); /* Serial never splits keyboard token. */
    uart_status=0xff; assert(console_input()==-1); /* Absent UART preserved. */
    enqueue_scan(0xe0,0); enqueue_scan(0x48,0x20); /* AUX does not consume prefix. */
    enqueue_scan(0x50,0); assert(console_input()==27);
    assert(console_input()=='['); assert(console_input()=='B');
    enqueue_scan(0x2a,0); enqueue_scan(0x1e,0xc0); enqueue_scan(0x1e,0);
    assert(console_input()=='a'); assert(!keyboard.modifiers);
    for (unsigned i=0; i<17; ++i) enqueue_scan(0x80,0);
    unsigned before=hardware_at; assert(console_input()==-1);
    assert(hardware_at-before==16); assert(console_input()==-1);
    assert(hardware_at==hardware_count && flushes);
}
int main(void) {
    characters(); modifier_lifetimes(); navigation_and_functions(); keypad_and_pause();
    overflow_and_recovery(); random_streams(); graphical_edges(); adapter();
    puts("keyboard checks passed: US text/control/Alt, independent modifiers, lock repeats, navigation/F1-F12, keypad, E0/E1/fake shift, atomic overflow/wrap, 500000-byte fuzz, i8042/UART adapter");
}
