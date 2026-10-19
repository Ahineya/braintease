/* SPDX-License-Identifier: GPL-2.0-only */
/* Shared /dev/urandom implementation. Kernel-local staging keeps RVM physical
 * device addresses separate from user MMU mappings and their COW pages. */
#include <vm_devices.h>
static int entropy_read(void) {
    unsigned char octets[VM_HOST_RANDOM_MAX];
    unsigned n=udata.u_count>VM_HOST_RANDOM_MAX?VM_HOST_RANDOM_MAX:(unsigned)udata.u_count;
    unsigned status;irqflags_t irq;
    if(!n)return 0;
    if(!udata.u_sysio&&!valaddr_w(udata.u_base,n,n))return -1;
    irq=di();status=vm_host_random(octets,n);irqrestore(irq);
    if(status!=VM_DEVICE_OK){udata.u_error=status==VM_DEVICE_UNSUPPORTED||status==VM_DEVICE_UNKNOWN_COMMAND?ENOSYS:EIO;return -1;}
    if(udata.u_sysio)memcpy(udata.u_base,octets,n);
    else if(uput(octets,udata.u_base,n))return -1;
    return n;
}
