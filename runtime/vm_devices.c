#include <vm_devices.h>
#include <mmio.h>

static unsigned short submit(unsigned short command,
                             volatile unsigned short *descriptor) {
    unsigned long address = (unsigned long)descriptor;
    mmio_write(MMIO_DEVICE_DESC_BANK, (unsigned short)(address >> 16));
    mmio_write(MMIO_DEVICE_DESC_OFF, (unsigned short)address);
    mmio_write(MMIO_DEVICE_COMMAND, command);
    /* NEXT_TICK completes before the following guest instruction is executed.
     * No guest polling loop or host-time assumption belongs in this helper. */
    return mmio_read(MMIO_DEVICE_STATUS);
}

static unsigned long join_words(unsigned short lo, unsigned short hi) {
    return (unsigned long)lo | ((unsigned long)hi << 16);
}

unsigned short vm_devices_version(void) {
    return mmio_read(MMIO_DEVICE_VERSION);
}

unsigned short vm_devices_caps(VmDeviceCaps *caps) {
    volatile unsigned short descriptor[4];
    unsigned short status;
    descriptor[0] = VM_DEVICE_ABI_VERSION;
    descriptor[1] = 4;
    descriptor[2] = 0;
    descriptor[3] = 0;
    status = submit(VM_DEVICE_CAPS, descriptor);
    if (status == VM_DEVICE_OK) {
        caps->bits = descriptor[2];
        caps->key_count = descriptor[3];
    }
    return status;
}

unsigned short vm_present_rgb565(const unsigned short *pixels,
                                unsigned short width, unsigned short height,
                                unsigned short stride) {
    volatile unsigned short descriptor[8];
    unsigned long address = (unsigned long)pixels;
    descriptor[0] = VM_DEVICE_ABI_VERSION;
    descriptor[1] = 8;
    descriptor[2] = width;
    descriptor[3] = height;
    descriptor[4] = stride;
    descriptor[5] = (unsigned short)(address >> 16);
    descriptor[6] = (unsigned short)address;
    descriptor[7] = 0;
    return submit(VM_DEVICE_PRESENT, descriptor);
}

static unsigned short clock_command(unsigned short command, VmClock *clock) {
    volatile unsigned short descriptor[8];
    unsigned short status;
    descriptor[0] = VM_DEVICE_ABI_VERSION;
    descriptor[1] = 8;
    descriptor[2] = 0;
    descriptor[3] = 0;
    descriptor[4] = 0;
    descriptor[5] = 0;
    descriptor[6] = 0;
    descriptor[7] = 0;
    status = submit(command, descriptor);
    if (status == VM_DEVICE_OK) {
        clock->tick = join_words(descriptor[2], descriptor[3]);
        clock->ms = join_words(descriptor[4], descriptor[5]);
        clock->hz = descriptor[6];
    }
    return status;
}

unsigned short vm_next_tick(VmClock *clock) {
    return clock_command(VM_DEVICE_NEXT_TICK, clock);
}

unsigned short vm_get_time(VmClock *clock) {
    return clock_command(VM_DEVICE_GET_TIME, clock);
}

unsigned short vm_input_pop(VmInputEvent *event) {
    volatile unsigned short descriptor[8];
    unsigned short status;
    unsigned short i;
    descriptor[0] = VM_DEVICE_ABI_VERSION;
    descriptor[1] = 8;
    for (i = 2; i < 8; ++i) descriptor[i] = 0;
    status = submit(VM_DEVICE_INPUT_POP, descriptor);
    if (status == VM_DEVICE_OK) {
        event->available = descriptor[2];
        event->key = descriptor[3];
        event->down = descriptor[4];
        event->tick = join_words(descriptor[5], descriptor[6]);
        event->overflow = descriptor[7];
    }
    return status;
}

unsigned short vm_input_state(VmInputState *state) {
    volatile unsigned short descriptor[19];
    unsigned short status;
    unsigned short i;
    descriptor[0] = VM_DEVICE_ABI_VERSION;
    descriptor[1] = 19;
    for (i = 2; i < 19; ++i) descriptor[i] = 0;
    status = submit(VM_DEVICE_INPUT_STATE, descriptor);
    if (status == VM_DEVICE_OK) {
        state->overflow = descriptor[2];
        for (i = 0; i < VM_KEY_STATE_WORDS; ++i) {
            state->held[i] = descriptor[3 + i];
        }
    }
    return status;
}

int vm_key_is_down(const VmInputState *state, unsigned short key) {
    if (key >= VM_KEY_COUNT) return 0;
    return (state->held[key >> 4] & (1U << (key & 15))) != 0;
}
