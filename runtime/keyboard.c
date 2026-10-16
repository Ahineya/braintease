#include <vm_devices.h>
/* The directional convenience API uses ordinary USB HID usages internally. */
int key_pressed(unsigned short key_usage) {
    VmInputState state;
    return vm_input_state(&state) == VM_DEVICE_OK && vm_key_is_down(&state,key_usage);
}
int key_up_pressed(void) { return key_pressed(VM_KEY_UP); }
int key_down_pressed(void) { return key_pressed(VM_KEY_DOWN); }
int key_left_pressed(void) { return key_pressed(VM_KEY_LEFT); }
int key_right_pressed(void) { return key_pressed(VM_KEY_RIGHT); }
int key_z_pressed(void) { return key_pressed(VM_KEY_Z); }
int key_x_pressed(void) { return key_pressed(VM_KEY_X); }
