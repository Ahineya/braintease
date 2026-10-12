#include <vm_devices.h>
#include <mmio.h>
#include <stdio.h>

static unsigned short pixels[12];

static int check_event(unsigned short key, unsigned short down,
                       unsigned long tick) {
    VmInputEvent event;
    if (vm_input_pop(&event) != VM_DEVICE_OK) return 0;
    return event.available && event.key == key && event.down == down &&
           event.tick == tick && !event.overflow;
}

static int fail(unsigned short code) {
    printf("input replay failure %u\n", code);
    return 1;
}

int main(void) {
    VmClock clock;
    VmInputEvent event;
    VmInputState keys;
    unsigned short tick;
    unsigned short i;
    unsigned short color;
    display_set_mode(DISP_MODE_RGB565_EXT);
    for (tick = 1; tick <= 3; ++tick) {
        if (vm_next_tick(&clock) != VM_DEVICE_OK) return fail(1);
        if (clock.tick != (unsigned long)tick) return fail(2);
        if (tick == 1) {
            if (!check_event(VM_KEY_W, 1, 1UL)) return fail(3);
        } else if (tick == 2) {
            if (!check_event(VM_KEY_W, 0, 2UL)) return fail(4);
            if (!check_event(VM_KEY_ENTER, 1, 2UL)) return fail(5);
        } else {
            if (!check_event(VM_KEY_ENTER, 0, 3UL)) return fail(6);
        }
        if (vm_input_pop(&event) != VM_DEVICE_OK || event.available) return fail(7);
        if (vm_input_state(&keys) != VM_DEVICE_OK || keys.overflow) return fail(8);
        if (vm_key_is_down(&keys, VM_KEY_W) != (tick == 1)) return fail(9);
        if (vm_key_is_down(&keys, VM_KEY_ENTER) != (tick == 2)) return fail(10);
        color = tick == 1 ? 0xf800U : (tick == 2 ? 0x07e0U : 0x001fU);
        for (i = 0; i < 12; ++i) pixels[i] = color;
        /* Four visible cells and two padding cells per row. */
        pixels[4] = 0xffffU;
        pixels[5] = 0xffffU;
        pixels[10] = 0xffffU;
        pixels[11] = 0xffffU;
        if (vm_present_rgb565(pixels, 4, 2, 6) != VM_DEVICE_OK) return fail(11);
    }
    puts("input replay ok");
    return 0;
}
