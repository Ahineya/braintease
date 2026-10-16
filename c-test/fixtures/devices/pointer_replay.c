#include <vm_devices.h>
#include <mmio.h>
#include <stdio.h>

static int fail(unsigned short code) { puts("pointer replay failure"); return code; }
int main(void) {
    VmInputEvent events[4];
    VmInputBatch batch;
    VmInputState state;
    VmClock clock;
    unsigned short i;
    if (vm_input_configure(VM_POINTER_RELATIVE) != VM_DEVICE_OK) return fail(1);
    if (vm_input_state(&state) != VM_DEVICE_OK || state.pointer_mode != VM_POINTER_RELATIVE || state.flags) return fail(2);
    if (vm_input_read(events,4,&batch) != VM_DEVICE_OK || batch.count || batch.sequence) return fail(3);
    if (vm_next_tick(&clock) != VM_DEVICE_OK || clock.tick != 1UL) return fail(4);
    if (!key_pressed(VM_KEY_W)) return fail(5);
    if (vm_input_read(events,4,&batch) != VM_DEVICE_OK || batch.count != 4 || batch.overflow || batch.sequence != 8UL) return fail(6);
    for (i=0;i<4;++i) if (events[i].sequence != (unsigned long)i+1UL || events[i].tick != 1UL) return fail(7);
    if (events[0].type != VM_INPUT_FOCUS || events[1].type != VM_INPUT_ENTER || events[2].type != VM_INPUT_KEY) return fail(8);
    if (events[2].code != VM_KEY_W || !(events[2].flags & VM_INPUT_DOWN)) return fail(9);
    if (events[3].type != VM_INPUT_MOTION || events[3].x != -70000L || events[3].y != 90000L) return fail(10);
    /* Normal state snapshots preserve the remaining four events. */
    if (vm_input_state(&state) != VM_DEVICE_OK || state.sequence != 8UL || state.buttons != 1 || state.x != 123451L || state.y != -345671L) return fail(11);
    if (!(state.flags & VM_INPUT_CAPTURED)) return fail(12);
    if (vm_input_read(events,4,&batch) != VM_DEVICE_OK || batch.count != 4) return fail(13);
    if (events[0].type != VM_INPUT_BUTTON || events[0].x != 123456L || events[0].y != -345678L || events[0].buttons != 1) return fail(14);
    if (events[1].type != VM_INPUT_MOTION || !(events[1].flags & VM_INPUT_RELATIVE) || events[1].dx != -5L || events[1].dy != 7L) return fail(15);
    if (events[2].type != VM_INPUT_WHEEL || events[2].wheel_x != -32768L || events[2].wheel_y != 98304L) return fail(16);
    if (events[3].type != VM_INPUT_CAPTURE || events[3].sequence != 8UL) return fail(17);
    if (vm_next_tick(&clock) != VM_DEVICE_OK || clock.tick != 2UL) return fail(18);
    if (vm_input_read(events,4,&batch) != VM_DEVICE_OK || batch.count != 4 || batch.sequence != 12UL) return fail(19);
    if (events[0].type != VM_INPUT_FOCUS || events[1].type != VM_INPUT_KEY || events[2].type != VM_INPUT_BUTTON || events[3].type != VM_INPUT_CAPTURE) return fail(20);
    for (i=0;i<4;++i) if (events[i].sequence != (unsigned long)i+9UL || events[i].tick != 2UL || (events[i].flags & VM_INPUT_DOWN)) return fail(21);
    if (vm_input_state(&state) != VM_DEVICE_OK || state.buttons || state.flags != VM_INPUT_INSIDE || vm_key_is_down(&state,VM_KEY_W)) return fail(22);
    for (i=10;i<=15;++i) { mmio_write(i,123); if (mmio_read(i)) return fail(23); }
    if (vm_get_time(&clock) != VM_DEVICE_OK || clock.tick != 2UL) return fail(24);
    if (vm_input_configure(VM_POINTER_ABSOLUTE) != VM_DEVICE_OK) return fail(25);
    puts("pointer replay ok");
    return 0;
}
