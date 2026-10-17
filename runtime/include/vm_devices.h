#ifndef VM_DEVICES_H
#define VM_DEVICES_H

#include <mmio_constants.h>

/* Device ABI 1. A descriptor word is one 16-bit C byte on Ripple.
 * Calls return VM_DEVICE_OK on success; output structs change only on success.
 * The caller owns its descriptor/framebuffer until the call returns. */
#define VM_DEVICE_ABI_VERSION 1
#define VM_DEVICE_OK 0
#define VM_DEVICE_UNKNOWN_COMMAND 1
#define VM_DEVICE_BAD_VERSION 2
#define VM_DEVICE_BAD_DESCRIPTOR 3
#define VM_DEVICE_BOUNDS 4
#define VM_DEVICE_BAD_MODE 5
#define VM_DEVICE_INPUT_OVERFLOW 6
#define VM_DEVICE_UNSUPPORTED 7
#define VM_DEVICE_WOULD_BLOCK 8
#define VM_DEVICE_BAD_STATE 9
#define VM_DEVICE_IO_ERROR 10
#define VM_DEVICE_PENDING 0x8000U

#define VM_DEVICE_PRESENT 1
#define VM_DEVICE_CAPS 2
#define VM_DEVICE_NEXT_TICK 3
#define VM_DEVICE_GET_TIME 4
#define VM_DEVICE_INPUT_READ 5
#define VM_DEVICE_INPUT_STATE 6
#define VM_DEVICE_INPUT_CONFIGURE 7
#define VM_DEVICE_HOST_MONOTONIC 8
#define VM_DEVICE_HOST_REALTIME 9
#define VM_DEVICE_HOST_IDLE 10
#define VM_DEVICE_HOST_TERMINAL 11
#define VM_HOST_IDLE_MAX_MS 10U
#define VM_INPUT_EVENT_CELLS 20
#define VM_INPUT_BATCH_MAX 64
#define VM_INPUT_KEY 1
#define VM_INPUT_MOTION 2
#define VM_INPUT_BUTTON 3
#define VM_INPUT_WHEEL 4
#define VM_INPUT_ENTER 5
#define VM_INPUT_FOCUS 6
#define VM_INPUT_CAPTURE 7
#define VM_INPUT_CLOSE 8
#define VM_INPUT_DOWN 1
#define VM_INPUT_RELATIVE 2
#define VM_INPUT_INSIDE 4
#define VM_INPUT_FOCUSED 8
#define VM_INPUT_CAPTURED 16
#define VM_POINTER_ABSOLUTE 0
#define VM_POINTER_RELATIVE 1
#define VM_BUTTON_LEFT 0
#define VM_BUTTON_RIGHT 1
#define VM_BUTTON_MIDDLE 2
#define VM_BUTTON_AUX1 3
#define VM_BUTTON_AUX2 4

#define VM_CAP_RGB565_EXT 0x0001U
#define VM_CAP_CLOCK 0x0002U
#define VM_CAP_INPUT_EVENTS 0x0004U
#define VM_CAP_INPUT_STATE 0x0008U
/* Live input can be polled before/between ticks without advancing the clock. */
#define VM_CAP_INPUT_POLL 0x0010U
#define VM_CAP_NETWORK 0x0020U
#define VM_CAP_POINTER 0x0040U
#define VM_CAP_PCM 0x0080U
#define VM_CAP_MUSIC 0x0100U
#define VM_CAP_HOST_CLOCK 0x1000U
#define VM_CAP_HOST_IDLE 0x2000U
#define VM_CAP_HOST_TERMINAL 0x4000U
#define VM_KEY_COUNT 256
#define VM_KEY_STATE_WORDS 16

/* USB HID keyboard usage IDs, independent of host keyboard APIs. */
#define VM_KEY_NONE 0
#define VM_KEY_A 4
#define VM_KEY_B 5
#define VM_KEY_C 6
#define VM_KEY_D 7
#define VM_KEY_E 8
#define VM_KEY_F 9
#define VM_KEY_G 10
#define VM_KEY_H 11
#define VM_KEY_I 12
#define VM_KEY_J 13
#define VM_KEY_K 14
#define VM_KEY_L 15
#define VM_KEY_M 16
#define VM_KEY_N 17
#define VM_KEY_O 18
#define VM_KEY_P 19
#define VM_KEY_Q 20
#define VM_KEY_R 21
#define VM_KEY_S 22
#define VM_KEY_T 23
#define VM_KEY_U 24
#define VM_KEY_V 25
#define VM_KEY_W 26
#define VM_KEY_X 27
#define VM_KEY_Y 28
#define VM_KEY_Z 29
#define VM_KEY_1 30
#define VM_KEY_2 31
#define VM_KEY_3 32
#define VM_KEY_4 33
#define VM_KEY_5 34
#define VM_KEY_6 35
#define VM_KEY_7 36
#define VM_KEY_8 37
#define VM_KEY_9 38
#define VM_KEY_0 39
#define VM_KEY_ENTER 40
#define VM_KEY_ESCAPE 41
#define VM_KEY_BACKSPACE 42
#define VM_KEY_TAB 43
#define VM_KEY_SPACE 44
#define VM_KEY_MINUS 45
#define VM_KEY_EQUALS 46
#define VM_KEY_LEFT_BRACKET 47
#define VM_KEY_RIGHT_BRACKET 48
#define VM_KEY_BACKSLASH 49
#define VM_KEY_NON_US_HASH 50
#define VM_KEY_SEMICOLON 51
#define VM_KEY_APOSTROPHE 52
#define VM_KEY_GRAVE 53
#define VM_KEY_COMMA 54
#define VM_KEY_PERIOD 55
#define VM_KEY_SLASH 56
#define VM_KEY_CAPS_LOCK 57
#define VM_KEY_F1 58
#define VM_KEY_F2 59
#define VM_KEY_F3 60
#define VM_KEY_F4 61
#define VM_KEY_F5 62
#define VM_KEY_F6 63
#define VM_KEY_F7 64
#define VM_KEY_F8 65
#define VM_KEY_F9 66
#define VM_KEY_F10 67
#define VM_KEY_F11 68
#define VM_KEY_F12 69
#define VM_KEY_PRINT_SCREEN 70
#define VM_KEY_SCROLL_LOCK 71
#define VM_KEY_PAUSE 72
#define VM_KEY_INSERT 73
#define VM_KEY_HOME 74
#define VM_KEY_PAGE_UP 75
#define VM_KEY_DELETE 76
#define VM_KEY_END 77
#define VM_KEY_PAGE_DOWN 78
#define VM_KEY_RIGHT 79
#define VM_KEY_LEFT 80
#define VM_KEY_DOWN 81
#define VM_KEY_UP 82
#define VM_KEY_NUM_LOCK 83
#define VM_KEY_KP_DIVIDE 84
#define VM_KEY_KP_MULTIPLY 85
#define VM_KEY_KP_MINUS 86
#define VM_KEY_KP_PLUS 87
#define VM_KEY_KP_ENTER 88
#define VM_KEY_KP_1 89
#define VM_KEY_KP_2 90
#define VM_KEY_KP_3 91
#define VM_KEY_KP_4 92
#define VM_KEY_KP_5 93
#define VM_KEY_KP_6 94
#define VM_KEY_KP_7 95
#define VM_KEY_KP_8 96
#define VM_KEY_KP_9 97
#define VM_KEY_KP_0 98
#define VM_KEY_KP_PERIOD 99
#define VM_KEY_NON_US_BACKSLASH 100
#define VM_KEY_APPLICATION 101
#define VM_KEY_LEFT_CTRL 224
#define VM_KEY_LEFT_SHIFT 225
#define VM_KEY_LEFT_ALT 226
#define VM_KEY_LEFT_GUI 227
#define VM_KEY_RIGHT_CTRL 228
#define VM_KEY_RIGHT_SHIFT 229
#define VM_KEY_RIGHT_ALT 230
#define VM_KEY_RIGHT_GUI 231

typedef struct VmDeviceCaps {
    unsigned short bits;
    unsigned short key_count;
} VmDeviceCaps;

typedef struct VmClock {
    unsigned long tick;
    unsigned long ms;
    unsigned short hz;
} VmClock;

typedef struct VmHostTime {
    unsigned long seconds_low, seconds_high;
    unsigned long nanoseconds, resolution_ns;
} VmHostTime;

typedef struct VmTerminalSize {
    unsigned short rows, columns;
} VmTerminalSize;

/* Wire-independent C objects. Absolute coordinates are signed guest pixels; relative deltas are raw
 * device units (X right, Y down). Auxiliary button IDs are host-dependent. Wheels
 * use signed 16.16 lines. Sequence numbers wrap modulo 2^32. */
typedef struct VmInputEvent {
    unsigned short type, flags, code, buttons;
    unsigned long sequence, tick;
    long x, y, dx, dy, wheel_x, wheel_y;
} VmInputEvent;
typedef struct VmInputBatch {
    unsigned short count, overflow;
    unsigned long sequence;
} VmInputBatch;
typedef struct VmInputState {
    unsigned short overflow, pointer_mode, buttons, flags;
    unsigned long sequence;
    long x, y;
    unsigned short held[VM_KEY_STATE_WORDS];
} VmInputState;

unsigned short vm_devices_version(void);
unsigned short vm_devices_caps(VmDeviceCaps *caps);

/* Select DISP_MODE_RGB565_EXT before presenting. Pixel stride is in cells;
 * the framebuffer may cross banks and is copied synchronously by the host. */
unsigned short vm_present_rgb565(const unsigned short *pixels,
                                unsigned short width, unsigned short height,
                                unsigned short stride);

/* NEXT_TICK yields to the host; GET_TIME is a pure read of logical time.
 * Neither presenting nor reading the clock advances a simulation tick. */
unsigned short vm_next_tick(VmClock *clock);
unsigned short vm_get_time(VmClock *clock);
/* Independent host clocks at 1ms resolution. Monotonic starts at the first
 * host-clock read after reset; checkpoints exclude time spent saved. Realtime
 * is Unix UTC time and may jump. Calls never advance logical simulation ticks.
 * Callers must serialize the mailbox, including against interrupt handlers. */
unsigned short vm_host_monotonic(VmHostTime *time);
unsigned short vm_host_realtime(VmHostTime *time);
/* Cooperatively yield (0) or idle the host for 1..10ms. No logical tick, IRQ,
 * input consumption or NIC attachment. The host scheduler may oversleep;
 * always reread monotonic time afterwards. Invalid requests never sleep.
 * This is a bounded polling fallback, not an event subscription. */
unsigned short vm_host_idle(unsigned short milliseconds);
/* Live host console dimensions; unsupported without an attached terminal.
 * Failure leaves the output object unchanged. Does not advance guest time. */
unsigned short vm_host_terminal_size(VmTerminalSize *size);
/* Input commands collect live events independently of NEXT_TICK. Failed
 * calls leave the output objects unchanged. Capacity must be 1..64. */
unsigned short vm_input_read(VmInputEvent *events, unsigned short capacity,
                             VmInputBatch *batch);
/* A snapshot acknowledges overflow and discards stale queued events when the
 * overflow flag was set; ordinary snapshots do not consume events. */
unsigned short vm_input_state(VmInputState *state);
/* Requests absolute/relative mode. Live capture is asynchronous: inspect
 * VM_INPUT_CAPTURED or VM_INPUT_CAPTURE events to determine host acceptance. */
unsigned short vm_input_configure(unsigned short mode);

int vm_key_is_down(const VmInputState *state, unsigned short key);

#endif
