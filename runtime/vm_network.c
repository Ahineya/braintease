#include <vm_network.h>
#include <mmio.h>

static unsigned short submit(unsigned short command, volatile unsigned short *d) {
    unsigned long address = (unsigned long)d;
    mmio_write(MMIO_DEVICE_DESC_BANK, (unsigned short)(address >> 16));
    mmio_write(MMIO_DEVICE_DESC_OFF, (unsigned short)address);
    mmio_write(MMIO_DEVICE_COMMAND, command);
    return mmio_read(MMIO_DEVICE_STATUS);
}
static unsigned long join(unsigned short lo, unsigned short hi) {
    return (unsigned long)lo | ((unsigned long)hi << 16);
}
unsigned short vm_net_info(unsigned short interface, VmNetInfo *info) {
    volatile unsigned short d[18];
    unsigned short i, status;
    d[0] = 1; d[1] = 18; d[2] = interface;
    for (i = 3; i < 18; ++i) d[i] = 0;
    status = submit(VM_DEVICE_NET_INFO, d);
    if (status != VM_DEVICE_OK) return status;
    info->link = d[3]; info->mtu = d[4]; info->max_frame = d[5];
    for (i = 0; i < 6; ++i) info->mac[i] = d[6 + i];
    info->rx_frames = d[12]; info->tx_frames = d[13];
    info->rx_octets = join(d[14], d[15]);
    info->tx_octets = join(d[16], d[17]);
    return status;
}
unsigned short vm_net_tx(unsigned short interface, const unsigned char *frame,
                         unsigned short length, unsigned short *outcome) {
    volatile unsigned short d[8];
    unsigned long address = (unsigned long)frame;
    unsigned short status;
    d[0] = 1; d[1] = 8; d[2] = interface;
    d[3] = (unsigned short)(address >> 16); d[4] = (unsigned short)address;
    d[5] = length; d[6] = 0; d[7] = 0;
    status = submit(VM_DEVICE_NET_TX, d);
    if (status == VM_DEVICE_OK) *outcome = d[6];
    return status;
}
unsigned short vm_net_rx(unsigned short interface, unsigned char *frame,
                         unsigned short capacity, VmNetReceive *result) {
    volatile unsigned short d[9];
    unsigned long address = (unsigned long)frame;
    unsigned short status;
    d[0] = 1; d[1] = 9; d[2] = interface;
    d[3] = (unsigned short)(address >> 16); d[4] = (unsigned short)address;
    d[5] = capacity; d[6] = 0; d[7] = 0; d[8] = 0;
    status = submit(VM_DEVICE_NET_RX, d);
    if (status == VM_DEVICE_OK) { result->length = d[6]; result->outcome = d[7]; }
    return status;
}
unsigned short vm_net_stats(unsigned short interface, VmNetStats *stats) {
    volatile unsigned short d[27];
    unsigned short i, status;
    d[0] = 1; d[1] = 27; d[2] = interface;
    for (i = 3; i < 27; ++i) d[i] = 0;
    status = submit(VM_DEVICE_NET_STATS, d);
    if (status != VM_DEVICE_OK) return status;
    stats->rx_queued = d[3]; stats->tx_queued = d[4];
    stats->rx_octets = join(d[5], d[6]); stats->tx_octets = join(d[7], d[8]);
    stats->rx_received = join(d[9], d[10]);
    stats->rx_delivered = join(d[11], d[12]);
    stats->rx_dropped = join(d[13], d[14]);
    stats->tx_accepted = join(d[15], d[16]);
    stats->tx_sent = join(d[17], d[18]);
    stats->tx_failed = join(d[19], d[20]);
    stats->tx_would_block = join(d[21], d[22]);
    stats->rx_oversized = join(d[23], d[24]);
    stats->backend_errors = join(d[25], d[26]);
    return status;
}
