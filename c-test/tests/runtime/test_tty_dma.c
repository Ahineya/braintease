#include <mmio.h>
#include <vm_devices.h>
#include <stdio.h>
int main(void) {
    unsigned short done = 99;
    char letters[3];
    letters[0] = 'Y'; letters[1] = 'Y'; letters[2] = 'Y';
    putchar('Y');
    if (vm_tty_write(letters, 3, &done) != VM_DEVICE_OK || done != 3) return 1;
    putchar(done == 3 ? 'Y' : 'N');
    puts("Y");
    printf("%s:%d\n", "YY", 42);
    return 0;
}
