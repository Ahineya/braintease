#include <vm_devices.h>
#include <mmio.h>
#include <stdlib.h>
#include <stdio.h>

static int fail(unsigned short code) {
    printf("vm device failure %u\n", code);
    return 1;
}

int main(void) {
    VmDeviceCaps caps;
    VmClock before;
    VmClock after;
    VmClock again;
    VmInputEvent event;
    VmInputState keys;
    unsigned short *pixels;
    unsigned short *cursor;
    unsigned short i;

    if (vm_devices_version() != VM_DEVICE_ABI_VERSION) return fail(1);
    if (vm_devices_caps(&caps) != VM_DEVICE_OK) return fail(2);
    if (caps.bits != 15 || caps.key_count != VM_KEY_COUNT) return fail(3);
    if (vm_get_time(&before) != VM_DEVICE_OK) return fail(4);
    if (before.tick != 0UL || before.ms != 0UL || before.hz == 0) return fail(5);
    if (vm_get_time(&again) != VM_DEVICE_OK) return fail(6);
    if (again.tick != before.tick || again.ms != before.ms) return fail(7);
    if (vm_next_tick(&after) != VM_DEVICE_OK) return fail(8);
    if (after.tick != 1UL || after.hz != before.hz) return fail(9);
    if (after.ms != 1000UL / (unsigned long)after.hz) return fail(10);
    if (vm_get_time(&again) != VM_DEVICE_OK) return fail(11);
    if (again.tick != after.tick || again.ms != after.ms) return fail(12);

    /* Run without input replay: the event queue and held bitmap start empty. */
    if (vm_input_pop(&event) != VM_DEVICE_OK) return fail(13);
    if (event.available || event.overflow) return fail(14);
    if (vm_input_state(&keys) != VM_DEVICE_OK) return fail(15);
    if (keys.overflow) return fail(16);
    for (i = 0; i < VM_KEY_STATE_WORDS; ++i) {
        if (keys.held[i] != 0) return fail(17);
    }
    if (vm_key_is_down(&keys, VM_KEY_ENTER)) return fail(18);
    if (vm_key_is_down(&keys, VM_KEY_COUNT)) return fail(19);

    /* 64,000 cells from malloc span a bank boundary and exceed mode 3's
     * resolution range. Pointer walking avoids a divide for every pixel. */
    pixels = malloc(64000UL * sizeof(unsigned short));
    if (!pixels) return fail(20);
    cursor = pixels;
    for (i = 0; i < 32000; ++i) *cursor++ = 0xf800U;
    for (i = 0; i < 32000; ++i) *cursor++ = 0x001fU;
    pixels[0] = 0xffffU;
    pixels[319] = 0x07e0U;
    pixels[63680UL] = 0xffe0U;
    pixels[63999UL] = 0x07ffU;

    if (vm_present_rgb565(pixels, 320, 200, 320) != VM_DEVICE_BAD_MODE) return fail(21);
    display_set_mode(DISP_MODE_RGB565_EXT);
    if (vm_present_rgb565(pixels, 320, 200, 320) != VM_DEVICE_OK) return fail(22);
    if (vm_get_time(&again) != VM_DEVICE_OK) return fail(23);
    if (again.tick != after.tick || again.ms != after.ms) return fail(24);
    free(pixels);
    puts("vm devices ok");
    return 0;
}
