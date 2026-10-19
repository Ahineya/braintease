/* Run the real helper loop with deterministic vmnet/socket faults, no sudo or
 * virtual interface. Build only on macOS alongside test_vmnet.py. */
#include <vmnet/vmnet.h>
#include <sys/socket.h>
#include <errno.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

static interface_ref fake_start(xpc_object_t config, dispatch_queue_t queue,
        vmnet_start_interface_completion_handler_t handler) {
    (void)config; (void)queue;
    xpc_object_t params = xpc_dictionary_create(NULL, NULL, 0);
    xpc_dictionary_set_string(params, vmnet_mac_address_key, "02:03:04:05:06:07");
    xpc_dictionary_set_uint64(params, vmnet_mtu_key, 1500);
    xpc_dictionary_set_uint64(params, vmnet_max_packet_size_key, 1514);
    handler(VMNET_SUCCESS, params);
    xpc_release(params);
    return (interface_ref)1;
}
static vmnet_return_t fake_read(interface_ref interface, struct vmpktdesc *packet, int *count) {
    (void)interface; (void)packet; *count = 0;
    static unsigned attempts;
    if (getenv("TEST_READ_BUSY") && attempts++ < 3) return VMNET_BUFFER_EXHAUSTED;
    return VMNET_SUCCESS;
}
static vmnet_return_t fake_write(interface_ref interface, struct vmpktdesc *packet, int *count) {
    (void)interface;
    static unsigned attempts;
    if (getenv("TEST_VMNET_FATAL")) return VMNET_FAILURE;
    const char *busy = getenv("TEST_VMNET_BUSY");
    if (busy && (!strcmp(busy, "forever") || attempts++ < 3)) {
        *count = 0;
        return !strcmp(busy, "zero") ? VMNET_SUCCESS : VMNET_BUFFER_EXHAUSTED;
    }
    fprintf(stderr, "TEST accepted %zu %u\n", packet->vm_pkt_size,
            ((unsigned char *)packet->vm_pkt_iov->iov_base)[14]);
    *count = 1; return VMNET_SUCCESS;
}
static vmnet_return_t fake_stop(interface_ref interface, dispatch_queue_t queue,
        vmnet_interface_completion_handler_t handler) {
    (void)interface; (void)queue; handler(VMNET_SUCCESS); return VMNET_SUCCESS;
}
static ssize_t fault_sendto(int fd, const void *data, size_t length, int flags,
        const struct sockaddr *peer, socklen_t peer_len) {
    static int injected;
    const char *fault = getenv("TEST_CONTROL_FAULT");
    int hello = getenv("TEST_HELLO_FAULT") != NULL;
    if (!injected && fault && ((hello && length == 13) ||
            (!hello && length == 5 && !memcmp(data, "RNET\2", 5)))) {
        injected = 1;
        errno = !strcmp(fault, "enobufs") ? ENOBUFS :
                !strcmp(fault, "eintr") ? EINTR : EAGAIN;
        return -1;
    }
    return sendto(fd, data, length, flags, peer, peer_len);
}
#define vmnet_start_interface fake_start
#define vmnet_read fake_read
#define vmnet_write fake_write
#define vmnet_stop_interface fake_stop
#define sendto fault_sendto
#include "vmnet.c"
