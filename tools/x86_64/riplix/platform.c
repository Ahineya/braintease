/* Bootstrap through the actual shared kernel and C16 filesystem. Context
 * mechanics use the native ABI; its services are supplied by our own kernel. */
#define main fuse_host_main
#include "../../../fuse-x/platform.c"
#undef main

int main(void) {
    VmHostTime now;
#ifdef RIPLIX_BOOTSTRAP
    fuse_running = 0;
#else
    fuse_running = 1;
#endif
    fuse_boot_parameters = fuse_running ? "rw" : "ro";
    kputs("Riplix: freestanding x86_64 kernel\n");
    fuse_disk_open("root", fuse_running);
    if (vm_host_monotonic(&now) != VM_DEVICE_OK) panic("x86_64: platform clock unavailable");
    host_last = now.seconds_low * 100UL + now.nanoseconds / 10000000UL;
    clock_ready = 1;
    rcc_hosted_bind_syscall(fuse_image_syscall);
    rcc_hosted_bind_checkpoint(fuse_checkpoint);
    fuzix_main();
    panic("x86_64: kernel initialization returned");
    return 1;
}
