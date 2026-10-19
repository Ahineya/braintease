/* SPDX-License-Identifier: GPL-2.0-only */
/* Staging is kernel-local: never submit a user's virtual address to MMIO. */
#include <vm_crypto.h>
static void crypto_clear(void *packet,unsigned n) {
    volatile unsigned char *p=packet;while(n--)*p++=0;
}
static int crypto_gcm_ioctl(char *pointer) {
    struct vm_crypto_gcm_request packet;
    unsigned status,cells=VM_CRYPTO_GCM_HEADER;
    irqflags_t irq;
    if(uget(pointer,&packet,cells))return -1;
    if(packet.version!=2||packet.reserved||packet.data_len>VM_CRYPTO_GCM_MAX) {
        crypto_clear(&packet,cells);udata.u_error=EINVAL;return -1;
    }
    cells+=packet.data_len;
    if(!valaddr_w(pointer,cells,cells)||
       (packet.data_len&&uget(pointer+VM_CRYPTO_GCM_HEADER,packet.data,packet.data_len))) {
        crypto_clear(&packet,cells);return -1;
    }
    irq=di();status=vm_crypto_gcm(&packet);irqrestore(irq);
    if(status!=VM_DEVICE_OK) {
        crypto_clear(&packet,cells);
        udata.u_error=status==VM_DEVICE_UNKNOWN_COMMAND||status==VM_DEVICE_UNSUPPORTED?ENOSYS:
            status==VM_DEVICE_BAD_DESCRIPTOR||status==VM_DEVICE_BOUNDS?EINVAL:
            status==VM_DEVICE_BAD_STATE?EACCES:EIO;
        return -1;
    }
    status=uput(&packet,pointer,cells);
    crypto_clear(&packet,cells);return status;
}
static int crypto_ioctl(uarg_t request, char *pointer) {
    struct vm_crypto_request packet;
    unsigned status;
    irqflags_t irq;
    if(request==RIPPLE_CRYPTO_GCM_IOCTL)return crypto_gcm_ioctl(pointer);
    if(request!=RIPPLE_CRYPTO_IOCTL){udata.u_error=ENOTTY;return -1;}
    if(!valaddr_w(pointer,sizeof(packet),sizeof(packet)) ||
       uget(pointer,&packet,sizeof(packet))){crypto_clear(&packet,sizeof(packet));return -1;}
    irq=di();status=vm_crypto(&packet);irqrestore(irq);
    if(status!=VM_DEVICE_OK) {
        crypto_clear(&packet,sizeof(packet));
        udata.u_error=status==VM_DEVICE_UNKNOWN_COMMAND||status==VM_DEVICE_UNSUPPORTED?ENOSYS:
            status==VM_DEVICE_BAD_DESCRIPTOR||status==VM_DEVICE_BOUNDS?EINVAL:EIO;
        return -1;
    }
    status=uput(&packet,pointer,sizeof(packet));
    crypto_clear(&packet,sizeof(packet));
    return status;
}
