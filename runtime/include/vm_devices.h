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
#define VM_DEVICE_PENDING 0x8000U

#define VM_DEVICE_PRESENT 1
#define VM_DEVICE_CAPS 2
#define VM_DEVICE_NEXT_TICK 3
#define VM_DEVICE_GET_TIME 4
#define VM_DEVICE_INPUT_POP 5
#define VM_DEVICE_INPUT_STATE 6

#define VM_CAP_RGB565_EXT 0x0001U
#define VM_CAP_CLOCK 0x0002U
#define VM_CAP_INPUT_EVENTS 0x0004U
#define VM_CAP_INPUT_STATE 0x0008U
/* Live input can be polled before/between ticks without advancing the clock. */
#define VM_CAP_INPUT_POLL 0x0010U
#define VM_CAP_NETWORK 0x0020U
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

typedef struct VmInputEvent {
    unsigned short available;
    unsigned short key;
    unsigned short down;
    unsigned long tick;
    unsigned short overflow;
} VmInputEvent;

typedef struct VmInputState {
    unsigned short overflow;
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
/* Input commands collect live keyboard transitions at the current logical tick
 * (which may be zero), independently of NEXT_TICK. Scheduled replay events
 * remain tied to tick grants. Check VM_CAP_INPUT_POLL on older ABI-1 hosts. */
unsigned short vm_input_pop(VmInputEvent *event);

/* Returns held keys and the sticky queue-overflow flag, then clears the flag.
 * After overflow, it also discards queued stale events: resynchronize from this
 * snapshot. Without overflow, the event queue is left intact. */
unsigned short vm_input_state(VmInputState *state);
int vm_key_is_down(const VmInputState *state, unsigned short key);

#endif
