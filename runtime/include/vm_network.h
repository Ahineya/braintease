#ifndef VM_NETWORK_H
#define VM_NETWORK_H
#include <vm_devices.h>

/* Ethernet NIC 0, through the existing DEVICE_* mailbox. No extra MMIO pins.
 * Each eight-bit frame octet occupies one C byte / 16-bit guest cell.
 * Functions return mailbox status; inspect outcome only on VM_DEVICE_OK. */
#define VM_DEVICE_NET_INFO 0x0100U
#define VM_DEVICE_NET_TX 0x0101U
#define VM_DEVICE_NET_RX 0x0102U
#define VM_DEVICE_NET_STATS 0x0103U
#define VM_NET_OK 0
#define VM_NET_EMPTY 1
#define VM_NET_WOULD_BLOCK 2
#define VM_NET_BUFFER_TOO_SMALL 3
#define VM_NET_LINK_DOWN 4

typedef struct VmNetInfo {
    unsigned short link;
    unsigned short mtu;
    unsigned short max_frame;
    unsigned char mac[6];
    unsigned short rx_frames;
    unsigned short tx_frames;
    unsigned long rx_octets;
    unsigned long tx_octets;
} VmNetInfo;

typedef struct VmNetReceive {
    unsigned short length;
    unsigned short outcome;
} VmNetReceive;

typedef struct VmNetStats {
    unsigned short rx_queued;
    unsigned short tx_queued;
    unsigned long rx_octets;
    unsigned long tx_octets;
    unsigned long rx_received;
    unsigned long rx_delivered;
    unsigned long rx_dropped;
    unsigned long tx_accepted;
    unsigned long tx_sent;
    unsigned long tx_failed;
    unsigned long tx_would_block;
    unsigned long rx_oversized;
    unsigned long backend_errors;
} VmNetStats;

unsigned short vm_net_info(unsigned short interface, VmNetInfo *info);
/* OK outcome means copied into the host queue, not delivered remotely. */
unsigned short vm_net_tx(unsigned short interface, const unsigned char *frame,
                         unsigned short length, unsigned short *outcome);
/* EMPTY/LINK_DOWN return length 0. BUFFER_TOO_SMALL returns the required
 * length, leaves payload untouched, and retains the queued frame for retry. */
unsigned short vm_net_rx(unsigned short interface, unsigned char *frame,
                         unsigned short capacity, VmNetReceive *result);
/* Counters saturate at 0xffffffff; reading does not reset them. */
unsigned short vm_net_stats(unsigned short interface, VmNetStats *stats);
#endif
