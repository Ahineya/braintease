/* macOS Ethernet attachment for RNET v1. This helper never interprets IP/ICMP.
 * Build with scripts/build-net-vmnet.sh. vmnet requires host authorization. */
#include <vmnet/vmnet.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stddef.h>

static volatile sig_atomic_t running = 1;
static FILE *capture;
static void stop(int signal) { (void)signal; running = 0; }
static void fail(const char *message) { perror(message); exit(1); }
static void pcap(const unsigned char *frame, size_t length) {
    if (!capture) return;
    struct timespec t; if (clock_gettime(CLOCK_REALTIME, &t)) fail("capture clock");
    uint32_t header[4] = {(uint32_t)t.tv_sec, (uint32_t)(t.tv_nsec / 1000), (uint32_t)length, (uint32_t)length};
    if (fwrite(header, sizeof(header), 1, capture) != 1 || fwrite(frame, length, 1, capture) != 1 || fflush(capture)) fail("capture write");
}
static double now(void) {
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC, &t)) fail("clock_gettime");
    return (double)t.tv_sec + (double)t.tv_nsec / 1000000000.0;
}
/* Runtime-owned temporary files are removed as the invoking user, after vmnet
 * has stopped. This also covers abrupt VM exit without language destructors. */
static void cleanup_managed(const char *socket_path, const char *executable) {
    char directory[108], helper[128], log[128];
    strcpy(directory, socket_path); /* socket path was checked against sun_path */
    char *last = strrchr(directory, '/');
    if (!last || strcmp(last, "/nic.sock")) return;
    *last = 0;
    unsigned long parent, serial; int end = 0;
    if (sscanf(directory, "/tmp/ripple-vmnet-%lu-%lu%n", &parent, &serial, &end) != 2 || directory[end] || !parent) return;
    snprintf(helper, sizeof(helper), "%s/helper", directory);
    if (strcmp(helper, executable)) return;
    snprintf(log, sizeof(log), "%s/helper.log", directory);
    if (geteuid() == 0) {
        if (!getenv("SUDO_UID") || !getenv("SUDO_GID")) return;
        if (setgid((gid_t)strtoul(getenv("SUDO_GID"), NULL, 10)) ||
            setuid((uid_t)strtoul(getenv("SUDO_UID"), NULL, 10))) fail("vmnet cleanup drop privileges");
    }
    if (unlink(helper) && errno != ENOENT) fail("cleanup embedded helper");
    if (unlink(log) && errno != ENOENT) fail("cleanup helper log");
    if (rmdir(directory) && errno != ENOENT) fail("cleanup helper directory");
}
static int send_message(int fd, const void *data, size_t length,
                         const struct sockaddr_un *peer, socklen_t peer_len) {
    if (sendto(fd, data, length, 0, (const struct sockaddr *)peer, peer_len) == (ssize_t)length) return 1;
    if (errno == ENOENT || errno == ECONNREFUSED || errno == EAGAIN || errno == EWOULDBLOCK) return 0;
    fail("network helper sendto"); return 0;
}
int main(int argc, char **argv) {
    int managed = argc > 2 && !strcmp(argv[argc - 1], "--managed");
    if (managed) --argc;
    if (argc < 2 || argc > 4 || (argc >= 3 && strcmp(argv[2], "host") && strcmp(argv[2], "shared"))) {
        fprintf(stderr, "Usage: %s SOCKET [host|shared] [capture.pcap] [--managed]\n", argv[0]); return 2;
    }
    int shared = argc >= 3 && !strcmp(argv[2], "shared");
    if (argc == 4) {
        capture = fopen(argv[3], "wb"); if (!capture) fail("capture open");
        uint32_t header[6] = {0xa1b2c3d4, 0x00040002, 0, 0, 65535, 1};
        if (fwrite(header, sizeof(header), 1, capture) != 1) fail("capture header");
    }
    struct sockaddr_un address = { .sun_family = AF_UNIX };
    if (strlen(argv[1]) >= sizeof(address.sun_path)) { fprintf(stderr, "socket path too long\n"); return 2; }
    strcpy(address.sun_path, argv[1]);
    umask(0077);
    int fd = socket(AF_UNIX, SOCK_DGRAM, 0);
    if (fd < 0) fail("socket");
    /* Refuse existing paths rather than deleting somebody else's socket/file. */
    if (bind(fd, (struct sockaddr *)&address, sizeof(address))) fail("bind");
    if (geteuid() == 0 && getenv("SUDO_UID") && getenv("SUDO_GID")) {
        if (chown(argv[1], (uid_t)strtoul(getenv("SUDO_UID"), NULL, 10),
                  (gid_t)strtoul(getenv("SUDO_GID"), NULL, 10))) fail("socket chown");
    }
    if (fcntl(fd, F_SETFL, O_NONBLOCK)) fail("fcntl");
    signal(SIGINT, stop); signal(SIGTERM, stop);
    dispatch_queue_t queue = dispatch_queue_create("ripple.vmnet", DISPATCH_QUEUE_SERIAL);
    dispatch_semaphore_t ready = dispatch_semaphore_create(0);
    xpc_object_t config = xpc_dictionary_create(NULL, NULL, 0);
    xpc_dictionary_set_uint64(config, vmnet_operation_mode_key, shared ? VMNET_SHARED_MODE : VMNET_HOST_MODE);
    xpc_dictionary_set_uint64(config, vmnet_mtu_key, 1500);
    xpc_dictionary_set_string(config, vmnet_start_address_key, "192.168.64.1");
    xpc_dictionary_set_string(config, vmnet_end_address_key, "192.168.64.128");
    xpc_dictionary_set_string(config, vmnet_subnet_mask_key, "255.255.255.0");
    __block vmnet_return_t startup = VMNET_FAILURE;
    unsigned char hello_storage[13] = {'R','N','E','T',1};
    unsigned char *hello = hello_storage;
    __block size_t max_packet = 0;
    interface_ref interface = vmnet_start_interface(config, queue, ^(vmnet_return_t status, xpc_object_t params) {
        startup = status;
        if (status == VMNET_SUCCESS) {
            unsigned mac[6];
            const char *text = xpc_dictionary_get_string(params, vmnet_mac_address_key);
            uint64_t mtu = xpc_dictionary_get_uint64(params, vmnet_mtu_key);
            max_packet = xpc_dictionary_get_uint64(params, vmnet_max_packet_size_key);
            if (!text || sscanf(text, "%x:%x:%x:%x:%x:%x", &mac[0], &mac[1], &mac[2], &mac[3], &mac[4], &mac[5]) != 6 || mtu != 1500 || max_packet < 1514 || max_packet > 65535) {
                startup = VMNET_INVALID_ARGUMENT;
            } else {
                for (int i = 0; i < 6; ++i) hello[5+i] = (unsigned char)mac[i];
                hello[11] = 1500 >> 8; hello[12] = 1500 & 255;
            }
        }
        dispatch_semaphore_signal(ready);
    });
    if (!interface || dispatch_semaphore_wait(ready, dispatch_time(DISPATCH_TIME_NOW, 10 * NSEC_PER_SEC)) || startup != VMNET_SUCCESS) {
        fprintf(stderr, "vmnet startup failed (%u); run this helper with vmnet authorization (usually sudo).\n", (unsigned)startup);
        unlink(argv[1]); return 1;
    }
    fprintf(stderr, "vmnet %s ready: socket=%s guest=192.168.64.250 host/gateway=192.168.64.1 mtu=1500 mac=%02x:%02x:%02x:%02x:%02x:%02x\n",
        shared ? "shared" : "host", argv[1], hello[5], hello[6], hello[7], hello[8], hello[9], hello[10]);
    struct sockaddr_un peer = {0}; socklen_t peer_len = 0;
    double last_seen = 0;
    double started = now();
    if (managed) { puts("RNET READY"); fflush(stdout); }
    unsigned long rx = 0, tx = 0, dropped = 0;
    unsigned char *buffer = malloc(65536);
    if (!buffer) fail("malloc");
    int failed = 0;
    while (running) {
        /* Owned helpers disappear after detach or a dead client's heartbeat
         * timeout, even when a VM exits through a signal without Rust drops. */
        if (managed && ((!peer_len && now() - started > 15) ||
                        (peer_len && now() - last_seen > 5))) break;
        for (int n = 0; n < 64; ++n) {
            struct sockaddr_un from = {0}; socklen_t from_len = sizeof(from);
            ssize_t length = recvfrom(fd, buffer, 65536, 0, (struct sockaddr *)&from, &from_len);
            if (length < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) break;
                fail("recvfrom");
            }
            if (from_len > sizeof(from) || from_len <= offsetof(struct sockaddr_un, sun_path) ||
                !from.sun_path[0] || !memchr(from.sun_path, 0, sizeof(from.sun_path))) { ++dropped; continue; }
            if (length == 5 && !memcmp(buffer, "RNET\1", 5)) {
                if (!peer_len || now() - last_seen > 5 || !strcmp(peer.sun_path, from.sun_path)) {
                    peer = from; peer_len = from_len; last_seen = now();
                    if (!send_message(fd, hello, 13, &peer, peer_len)) { ++dropped; peer_len = 0; }
                } else if (!send_message(fd, "RNET\3", 5, &from, from_len)) ++dropped;
                continue;
            }
            if (!peer_len || strcmp(peer.sun_path, from.sun_path)) { ++dropped; continue; }
            if (length == 5 && !memcmp(buffer, "RNET\4", 5)) { peer_len = 0; if (managed) running = 0; continue; }
            if (length == 5 && !memcmp(buffer, "RNET\2", 5)) {
                last_seen = now();
                if (!send_message(fd, "RNET\2", 5, &peer, peer_len)) { ++dropped; peer_len = 0; }
                continue;
            }
            if (length < 14 || length > 1514) { ++dropped; continue; }
            struct iovec iov = { .iov_base = buffer, .iov_len = (size_t)length };
            struct vmpktdesc packet = { .vm_pkt_size = (size_t)length, .vm_pkt_iov = &iov, .vm_pkt_iovcnt = 1 };
            int count = 1;
            if (vmnet_write(interface, &packet, &count) != VMNET_SUCCESS || count != 1) {
                fprintf(stderr, "vmnet_write failed\n"); failed = 1; running = 0; break;
            }
            pcap(buffer, (size_t)length);
            ++tx;
        }
        for (int n = 0; n < 64 && running; ++n) {
            struct iovec iov = { .iov_base = buffer, .iov_len = max_packet };
            struct vmpktdesc packet = { .vm_pkt_size = max_packet, .vm_pkt_iov = &iov, .vm_pkt_iovcnt = 1 };
            int count = 1;
            if (vmnet_read(interface, &packet, &count) != VMNET_SUCCESS) {
                fprintf(stderr, "vmnet_read failed\n"); failed = 1; running = 0; break;
            }
            if (!count) break;
            pcap(buffer, packet.vm_pkt_size);
            if (!peer_len || now() - last_seen > 5 || packet.vm_pkt_size > 1514) { ++dropped; continue; }
            ssize_t sent = sendto(fd, buffer, packet.vm_pkt_size, 0, (struct sockaddr *)&peer, peer_len);
            if (sent == (ssize_t)packet.vm_pkt_size) ++rx;
            else if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == ENOENT || errno == ECONNREFUSED)) ++dropped;
            else fail("frame sendto");
        }
        struct timespec delay = { .tv_nsec = 1000000 }; nanosleep(&delay, NULL);
    }
    if (peer_len && failed) sendto(fd, "RNET\3", 5, 0, (struct sockaddr *)&peer, peer_len);
    vmnet_return_t stopping = vmnet_stop_interface(interface, queue, ^(vmnet_return_t status) {
        if (status != VMNET_SUCCESS) fprintf(stderr, "vmnet stop failed: %u\n", (unsigned)status);
        dispatch_semaphore_signal(ready);
    });
    if (stopping != VMNET_SUCCESS || dispatch_semaphore_wait(ready, dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC))) failed = 1;
    fprintf(stderr, "vmnet frames: guest_tx=%lu guest_rx=%lu dropped=%lu\n", tx, rx, dropped);
    free(buffer); close(fd); unlink(argv[1]);
    if (capture && fclose(capture)) failed = 1;
    if (managed && !failed) cleanup_managed(argv[1], argv[0]);
    return failed;
}
