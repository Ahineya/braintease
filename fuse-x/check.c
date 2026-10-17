/* SPDX-License-Identifier: GPL-2.0-only */
#include "host.h"
#include <kdata.h>
#include <printf.h>
#include <timer.h>
#include <vm_devices.h>
#include <string.h>

static arg_t call(unsigned number, unsigned long a, unsigned long b, unsigned long c) {
    udata.u_callno = number;
    udata.u_argn = a; udata.u_argn1 = b; udata.u_argn2 = c; udata.u_argn3 = 0;
    unix_syscall();
    if (udata.u_error) { kprintf("Fuse-X: syscall %d failed, errno %d\n", number, udata.u_error); exit(1); }
    return udata.u_retval;
}
void fuse_boot_check(void) {
    char *path = (char *)(PROGBASE + 1024);
    char *buffer = path + 256;
    int fd;
    arg_t count;
    VmHostTime before, after, realtime;
    VmClock logical;
    unsigned long start;
    unsigned i;
    if (!root || !init_process || init_process->p_pid != 1 || nproc != 1 ||
        strcmp((char *)(PROGBASE + 256), "/init")) panic("Fuse-X: bootstrap state");
    strcpy(path, "/");
    fd = call(1, (unsigned long)path, O_RDONLY, 0);
    count = call(7, fd, (unsigned long)buffer, DIR_LEN);
    if (count != DIR_LEN || buffer[1] != '.') panic("Fuse-X: root directory read");
    call(2, fd, 0, 0);
    kputs("Fuse-X: root mounted; PID 1 and kernel file syscalls verified\n");
    if (fuse_value) {
        strcpy(path, "/tmp/fuse-x-check");
        if (fuse_write_check) {
            strcpy(buffer, fuse_value);
            fd = call(1, (unsigned long)path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
            if (call(8, fd, (unsigned long)buffer, strlen(buffer)) != strlen(buffer)) panic("Fuse-X: short write");
            call(2, fd, 0, 0);
        }
        memset(buffer, 0, 1024);
        fd = call(1, (unsigned long)path, O_RDONLY, 0);
        count = call(7, fd, (unsigned long)buffer, 1024);
        call(2, fd, 0, 0);
        if (count != strlen(fuse_value) || memcmp(buffer, fuse_value, count)) panic("Fuse-X: persistent file contents");
        kputs("Fuse-X: file contents verified through FUZIX\n");
    }
    if (vm_host_monotonic(&before) != VM_DEVICE_OK) panic("Fuse-X: clock read");
    start = ticks.full;
    do {
        sync_clock();
        if (vm_host_monotonic(&after) != VM_DEVICE_OK) panic("Fuse-X: clock read");
    } while (ticks.full == start && after.seconds_low - before.seconds_low < 2);
    if (ticks.full == start) panic("Fuse-X: kernel clock advancement");
    if (vm_host_realtime(&realtime) != VM_DEVICE_OK || tod.high != realtime.seconds_high ||
        realtime.seconds_low - tod.low > 1UL) panic("Fuse-X: kernel realtime initialization");
    if (vm_get_time(&logical) != VM_DEVICE_OK || logical.tick != 0)
        panic("Fuse-X: host clocks changed logical time");
    kputs("Fuse-X: host clocks advance kernel time independently of instructions\n");
    kputs("Fuse-X: bootstrap check passed at native /init loader boundary\n");
    fuse_shutdown();
}

void fuse_shutdown(void) {
    unsigned i;
    udata.u_error = 0;
    sync();
    if (udata.u_error) panic("Fuse-X: filesystem sync failed");
    for (i = 0; i < NMOUNTS; i++)
        if (fs_tab[i].m_dev != NO_DEVICE && !(fs_tab[i].m_flags & MS_RDONLY))
            fs_tab[i].m_fs.s_fmod = FMOD_GO_CLEAN;
    sync();
    if (udata.u_error) panic("Fuse-X: clean filesystem sync failed");
    fuse_disk_close();
    kputs("Fuse-X: filesystem synced; session stopped\n");
    exit(0);
}
