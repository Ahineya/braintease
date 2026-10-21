/* Raw Ethernet NIC boundary. All buffers use native octets, never C16 DMA. */
#ifndef RIPLIX_NET_H
#define RIPLIX_NET_H
#include <stdint.h>
#define NIC_FRAME 1514U
#define NIC_FRAMES 64U
#define NIC_OCTETS 65536U
/* Mailbox outcomes match runtime/include/vm_network.h. */
#define NIC_OK 0
#define NIC_EMPTY 1
#define NIC_WOULD_BLOCK 2
#define NIC_TOO_SMALL 3
#define NIC_DOWN 4
typedef struct {
    unsigned link, rx_frames, tx_frames, rx_octets, tx_octets;
    uint8_t mac[6];
    uint32_t counters[9];
} NicInfo;
void nic_init(void);
void nic_info(NicInfo *);
unsigned nic_tx(const uint8_t *, unsigned);
unsigned nic_rx(uint8_t *, unsigned, unsigned *);
void nic_stop(void);
#endif
