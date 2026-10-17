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

static unsigned short host_clock_command(unsigned short command, VmHostTime *time) {
    volatile unsigned short descriptor[10];
    unsigned short i, status;
    if (vm_devices_version() != VM_DEVICE_ABI_VERSION) return VM_DEVICE_UNSUPPORTED;
    descriptor[0] = VM_DEVICE_ABI_VERSION; descriptor[1] = 10;
    for (i = 2; i < 10; ++i) descriptor[i] = 0;
    status = submit(command, descriptor);
    if (status == VM_DEVICE_OK) {
        time->seconds_low = join_words(descriptor[2], descriptor[3]);
        time->seconds_high = join_words(descriptor[4], descriptor[5]);
        time->nanoseconds = join_words(descriptor[6], descriptor[7]);
        time->resolution_ns = join_words(descriptor[8], descriptor[9]);
    }
    return status;
}

unsigned short vm_host_monotonic(VmHostTime *time) {
    return host_clock_command(VM_DEVICE_HOST_MONOTONIC, time);
}
unsigned short vm_host_realtime(VmHostTime *time) {
    return host_clock_command(VM_DEVICE_HOST_REALTIME, time);
}

unsigned short vm_host_idle(unsigned short milliseconds) {
    volatile unsigned short descriptor[4];
    if (milliseconds > VM_HOST_IDLE_MAX_MS) return VM_DEVICE_BAD_DESCRIPTOR;
    if (vm_devices_version() != VM_DEVICE_ABI_VERSION) return VM_DEVICE_UNSUPPORTED;
    descriptor[0] = VM_DEVICE_ABI_VERSION; descriptor[1] = 4;
    descriptor[2] = milliseconds; descriptor[3] = 0;
    return submit(VM_DEVICE_HOST_IDLE, descriptor);
}

unsigned short vm_host_terminal_size(VmTerminalSize *size) {
    volatile unsigned short descriptor[4];
    unsigned short status;
    if (vm_devices_version() != VM_DEVICE_ABI_VERSION) return VM_DEVICE_UNSUPPORTED;
    descriptor[0] = VM_DEVICE_ABI_VERSION; descriptor[1] = 4;
    descriptor[2] = descriptor[3] = 0;
    status = submit(VM_DEVICE_HOST_TERMINAL, descriptor);
    if (status == VM_DEVICE_OK) {
        size->rows = descriptor[2]; size->columns = descriptor[3];
    }
    return status;
}

static long signed_words(unsigned short lo, unsigned short hi) {
    unsigned long value = join_words(lo, hi);
    if (hi & 32768U) return -1L - (long)(0xffffffffUL - value);
    return (long)value;
}
unsigned short vm_input_read(VmInputEvent *events, unsigned short capacity,
                             VmInputBatch *batch) {
    volatile unsigned short descriptor[10];
    unsigned short words[VM_INPUT_BATCH_MAX * VM_INPUT_EVENT_CELLS];
    unsigned long address = (unsigned long)words;
    unsigned short status, i;
    if (!capacity || capacity > VM_INPUT_BATCH_MAX) return VM_DEVICE_BAD_DESCRIPTOR;
    descriptor[0] = VM_DEVICE_ABI_VERSION; descriptor[1] = 10;
    descriptor[2] = capacity; descriptor[3] = (unsigned short)(address >> 16);
    descriptor[4] = (unsigned short)address;
    for (i = 5; i < 10; ++i) descriptor[i] = 0;
    status = submit(VM_DEVICE_INPUT_READ, descriptor);
    if (status != VM_DEVICE_OK) return status;
    batch->count = descriptor[5]; batch->overflow = descriptor[6];
    batch->sequence = join_words(descriptor[7], descriptor[8]);
    for (i = 0; i < batch->count; ++i) {
        unsigned short *w = words + (unsigned long)i * VM_INPUT_EVENT_CELLS;
        events[i].type = w[0]; events[i].flags = w[1];
        events[i].code = w[2]; events[i].buttons = w[3];
        events[i].sequence = join_words(w[4],w[5]); events[i].tick = join_words(w[6],w[7]);
        events[i].x = signed_words(w[8],w[9]); events[i].y = signed_words(w[10],w[11]);
        events[i].dx = signed_words(w[12],w[13]); events[i].dy = signed_words(w[14],w[15]);
        events[i].wheel_x = signed_words(w[16],w[17]); events[i].wheel_y = signed_words(w[18],w[19]);
    }
    return status;
}
unsigned short vm_input_state(VmInputState *state) {
    volatile unsigned short descriptor[28];
    unsigned short status, i;
    descriptor[0] = VM_DEVICE_ABI_VERSION; descriptor[1] = 28;
    for (i = 2; i < 28; ++i) descriptor[i] = 0;
    status = submit(VM_DEVICE_INPUT_STATE, descriptor);
    if (status == VM_DEVICE_OK) {
        state->overflow = descriptor[2]; state->sequence = join_words(descriptor[3],descriptor[4]);
        state->pointer_mode = descriptor[5]; state->buttons = descriptor[6]; state->flags = descriptor[7];
        state->x = signed_words(descriptor[8],descriptor[9]); state->y = signed_words(descriptor[10],descriptor[11]);
        for (i = 0; i < VM_KEY_STATE_WORDS; ++i) state->held[i] = descriptor[12 + i];
    }
    return status;
}
unsigned short vm_input_configure(unsigned short mode) {
    volatile unsigned short descriptor[4];
    descriptor[0] = VM_DEVICE_ABI_VERSION; descriptor[1] = 4;
    descriptor[2] = mode; descriptor[3] = 0;
    return submit(VM_DEVICE_INPUT_CONFIGURE, descriptor);
}

int vm_key_is_down(const VmInputState *state, unsigned short key) {
    if (key >= VM_KEY_COUNT) return 0;
    return (state->held[key >> 4] & (1U << (key & 15))) != 0;
}
