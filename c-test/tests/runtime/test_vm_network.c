#include <vm_network.h>
#include <stdio.h>

static void check(int condition) { if (condition) putchar('Y'); else putchar('N'); }
static unsigned char frame[17];
int main(void) {
    VmDeviceCaps caps;
    VmNetInfo info;
    VmNetStats stats;
    VmNetReceive received;
    VmClock before, after;
    VmInputEvent event;
    VmInputBatch batch;
    unsigned short outcome = 999;
    check(vm_devices_caps(&caps) == 0 && (caps.bits & VM_CAP_NETWORK));
    check(vm_net_info(0, &info) == 0 && !info.link && info.mtu == 1500 && info.max_frame == 1514);
    check(info.rx_frames == 64 && info.tx_frames == 64 && info.rx_octets == 65536UL && info.tx_octets == 65536UL);
    check(vm_get_time(&before) == 0);
    check(vm_net_rx(0, frame, 17, &received) == 0 && received.outcome == VM_NET_LINK_DOWN && received.length == 0);
    check(vm_net_tx(0, frame, 17, &outcome) == 0 && outcome == VM_NET_LINK_DOWN);
    outcome = 999; frame[2] = 256;
    check(vm_net_tx(0, frame, 17, &outcome) == VM_DEVICE_BAD_DESCRIPTOR && outcome == 999);
    check(vm_net_info(1, &info) == VM_DEVICE_BAD_DESCRIPTOR);
    check(vm_input_read(&event, 1, &batch) == 0 && !batch.count);
    check(vm_get_time(&after) == 0 && after.tick == before.tick && after.ms == before.ms);
    check(vm_net_stats(0, &stats) == 0 && !stats.rx_received && !stats.tx_accepted && !stats.rx_queued && !stats.tx_queued);
    putchar('\n');
    return 0;
}
