/* Freestanding Ethernet/ARP/IPv4/ICMP demo. One octet per guest cell.
 * Defaults match tools/network/vmnet.c. Override configuration at compile time.
 * No host socket, DNS or ICMP implementation is called by this guest. */
#include <vm_network.h>
#include <stdio.h>

#ifndef NET_LOCAL_IP
#define NET_LOCAL_IP 192,168,64,250
#endif
#ifndef NET_TARGET_IP
#define NET_TARGET_IP 192,168,64,1
#endif
#ifndef NET_GATEWAY_IP
#define NET_GATEWAY_IP 192,168,64,1
#endif
#ifndef NET_MASK
#define NET_MASK 255,255,255,0
#endif
#ifndef PING_REQUESTS
#define PING_REQUESTS 1
#endif
#ifndef PING_RESPONSES
#define PING_RESPONSES 1
#endif
#ifndef PING_MONITOR
#define PING_MONITOR 0
#endif

static unsigned char local_ip[4] = {NET_LOCAL_IP};
static unsigned char target_ip[4] = {NET_TARGET_IP};
static unsigned char gateway_ip[4] = {NET_GATEWAY_IP};
static unsigned char mask[4] = {NET_MASK};
static unsigned char local_mac[6];
static unsigned char hop_ip[4];
static unsigned char hop_mac[6];
static unsigned char rx[1514];
static unsigned char tx[1514];
static unsigned short learned;
static unsigned short sent;
static unsigned short received;
static unsigned short responded;
static unsigned short rejected;
static unsigned short pending_length;

/* Keep the BF image small: the general printf object also contains floating
 * formatting. These logs need only literal text and unsigned decimal values. */
static void text(const char *s) { while (*s) putchar(*s++); }
static void number(unsigned long value) {
    unsigned char digits[10]; unsigned short count = 0;
    do { digits[count++] = '0' + value % 10UL; value /= 10UL; } while (value);
    while (count) putchar(digits[--count]);
}
static void ip_text(const unsigned char *ip) {
    unsigned short i;
    for (i = 0; i < 4; ++i) { if (i) putchar('.'); number(ip[i]); }
}
static int fail(const char *message, int code) {
    text("PING ERROR: "); puts(message); return code;
}
static unsigned short get16(const unsigned char *p) { return (unsigned short)(p[0] * 256U + p[1]); }
static void put16(unsigned char *p, unsigned short value) { p[0] = value / 256U; p[1] = value % 256U; }
static void copy(unsigned char *to, const unsigned char *from, unsigned short count) {
    while (count--) *to++ = *from++;
}
static int equal(const unsigned char *a, const unsigned char *b, unsigned short count) {
    while (count--) if (*a++ != *b++) return 0;
    return 1;
}
/* Carry on every addition keeps the hot loop in 16-bit arithmetic. The final
 * odd octet is the high octet of a zero-padded network word. */
static unsigned short checksum(const unsigned char *p, unsigned short length) {
    unsigned short sum = 0, word, before;
    while (length >= 2) {
        word = get16(p); before = sum; sum += word;
        if (sum < before) ++sum;
        p += 2; length -= 2;
    }
    if (length) {
        word = p[0] * 256U; before = sum; sum += word;
        if (sum < before) ++sum;
    }
    return (unsigned short)(65535U - sum);
}
static void ethernet(const unsigned char *destination, unsigned short type) {
    copy(tx, destination, 6); copy(tx + 6, local_mac, 6); put16(tx + 12, type);
}
static void ipv4(const unsigned char *destination, unsigned short payload_length) {
    unsigned short i;
    for (i = 14; i < 34; ++i) tx[i] = 0;
    tx[14] = 0x45; put16(tx + 16, payload_length + 20);
    put16(tx + 18, sent + responded + 1); tx[22] = 64; tx[23] = 1;
    copy(tx + 26, local_ip, 4); copy(tx + 30, destination, 4);
    put16(tx + 24, checksum(tx + 14, 20));
}
static int transmit(unsigned short length) {
    unsigned short outcome;
    /* Ethernet padding is zero and is not part of IP/ICMP length/checksum. */
    while (length < 60) tx[length++] = 0;
    if (vm_net_tx(0, tx, length, &outcome) != VM_DEVICE_OK) return -1;
    if (outcome == VM_NET_WOULD_BLOCK) { pending_length = length; return 0; }
    if (outcome != VM_NET_OK) return -1;
    pending_length = 0;
    return 1;
}
static int arp(unsigned short operation, const unsigned char *destination,
               const unsigned char *destination_ip) {
    unsigned short i;
    ethernet(destination, 0x0806);
    put16(tx + 14, 1); put16(tx + 16, 0x0800); tx[18] = 6; tx[19] = 4;
    put16(tx + 20, operation); copy(tx + 22, local_mac, 6); copy(tx + 28, local_ip, 4);
    for (i = 0; i < 6; ++i) tx[32 + i] = operation == 1 ? 0 : destination[i];
    copy(tx + 38, destination_ip, 4);
    return transmit(42);
}
static int request(void) {
    unsigned short i;
    ethernet(hop_mac, 0x0800); ipv4(target_ip, 25);
    tx[34] = 8; tx[35] = 0; tx[36] = 0; tx[37] = 0;
    put16(tx + 38, 0x5246); put16(tx + 40, sent + 1);
    for (i = 0; i < 17; ++i) tx[42+i] = i == 1 ? 255 : i;
    put16(tx + 36, checksum(tx + 34, 25));
    ++sent;
    return transmit(59);
}
static int handle(unsigned short length) {
    unsigned short type, operation, ihl, total, icmp_length, i, sequence;
    unsigned char broadcast[6] = {255,255,255,255,255,255};
    if (length < 14 || (!equal(rx, local_mac, 6) && !equal(rx, broadcast, 6))) return 0;
    type = get16(rx + 12);
    if (type == 0x0806) {
        if (length < 42 || get16(rx + 14) != 1 || get16(rx + 16) != 0x0800 || rx[18] != 6 || rx[19] != 4) { ++rejected; return 0; }
        operation = get16(rx + 20);
        if ((operation != 1 && operation != 2) || !equal(rx + 38, local_ip, 4) || !equal(rx + 6, rx + 22, 6)) return 0;
        if (equal(rx + 28, hop_ip, 4)) { copy(hop_mac, rx + 22, 6); learned = 1; }
        if (operation == 1) return arp(2, rx + 22, rx + 28) < 0 ? -1 : 0;
        return 0;
    }
    if (type != 0x0800) return 0;
    if (length < 34 || rx[14] / 16 != 4) { ++rejected; return 0; }
    ihl = (rx[14] % 16) * 4; total = get16(rx + 16);
    if (ihl < 20 || ihl > 60 || total < ihl + 8 || total > length - 14 ||
        (get16(rx + 20) & 0xbfffU) || rx[22] == 0 || rx[23] != 1 ||
        !equal(rx + 30, local_ip, 4) || checksum(rx + 14, ihl) != 0) { ++rejected; return 0; }
    icmp_length = total - ihl;
    if (rx[14 + ihl + 1] != 0 || checksum(rx + 14 + ihl, icmp_length) != 0) { ++rejected; return 0; }
    if (rx[14 + ihl] == 8) {
#if PING_MONITOR
        text("PING request from="); ip_text(rx + 26);
        text(" id="); number(get16(rx + 14 + ihl + 4));
        text(" seq="); number(get16(rx + 14 + ihl + 6));
        text(" octets="); number(icmp_length - 8); putchar('\n');
#endif
        ethernet(rx + 6, 0x0800); ipv4(rx + 26, icmp_length);
        copy(tx + 34, rx + 14 + ihl, icmp_length);
        tx[34] = 0; tx[36] = 0; tx[37] = 0;
        put16(tx + 36, checksum(tx + 34, icmp_length));
        ++responded;
        i = transmit(34 + icmp_length) < 0;
#if PING_MONITOR
        if (!i) puts(pending_length ? "PING reply waiting for TX space" : "PING reply queued");
#endif
        return i ? -1 : 0;
    }
    if (rx[14 + ihl] == 0 && equal(rx + 26, target_ip, 4) && get16(rx + 14 + ihl + 4) == 0x5246) {
        sequence = get16(rx + 14 + ihl + 6);
        if (icmp_length != 25 || sequence != received + 1 || sequence > sent) { ++rejected; return 0; }
        for (i = 0; i < 17; ++i) if (rx[14 + ihl + 8 + i] != (i == 1 ? 255 : i)) { ++rejected; return 0; }
        ++received;
        text("PING reply seq="); number(sequence); puts(" octets=17");
    }
    return 0;
}
int main(void) {
    VmDeviceCaps caps;
    VmNetInfo info;
    VmNetReceive result;
    VmNetStats stats;
    unsigned short i;
    unsigned char broadcast[6] = {255,255,255,255,255,255};
    if (vm_devices_version() != 1 || vm_devices_caps(&caps) != 0 || !(caps.bits & VM_CAP_NETWORK)) return fail("network device ABI unavailable", 1);
    if (vm_net_info(0, &info) != 0 || !info.link || info.max_frame > 1514) return fail("NIC unavailable, link down, or unsupported frame size", 2);
    copy(local_mac, info.mac, 6);
    copy(hop_ip, target_ip, 4);
    for (i = 0; i < 4; ++i) if ((local_ip[i] & mask[i]) != (target_ip[i] & mask[i])) { copy(hop_ip, gateway_ip, 4); break; }
#if PING_MONITOR
    /* Refresh neighbors after a restart assigns this static IP a new MAC.
     * This gratuitous ARP is constructed by the guest, like other ARP traffic. */
    if (arp(1, broadcast, local_ip) < 0) return fail("ARP announcement failed", 3);
    text("PING monitor local="); ip_text(local_ip); text(" mask="); ip_text(mask); putchar('\n');
    text("gateway="); ip_text(gateway_ip); text(" MAC=");
    for (i = 0; i < 6; ++i) {
        if (i) putchar(':');
        putchar("0123456789abcdef"[local_mac[i] / 16]);
        putchar("0123456789abcdef"[local_mac[i] % 16]);
    }
    puts("\nWaiting for pings; Ctrl-C to stop.");
#else
    text("PING ready local="); ip_text(local_ip); text(" target="); ip_text(target_ip); putchar('\n');
#endif
    if (PING_REQUESTS && arp(1, broadcast, hop_ip) < 0) return fail("ARP transmit failed", 3);
    while (PING_MONITOR || received < PING_REQUESTS || responded < PING_RESPONSES || pending_length) {
        if (pending_length) { if (transmit(pending_length) < 0) return fail("queued transmit retry failed", 4); continue; }
        if (learned && sent < PING_REQUESTS && received == sent) { if (request() < 0) return fail("echo request transmit failed", 5); continue; }
        if (vm_net_rx(0, rx, 1514, &result) != 0) return fail("RX descriptor rejected", 6);
        if (result.outcome == VM_NET_EMPTY) continue;
        if (result.outcome != VM_NET_OK) return fail("RX link down or buffer too small", 7);
        if (handle(result.length) < 0) return fail("echo/ARP reply transmit failed", 8);
    }
    /* TX acceptance only copies into a queue. Keep the process alive until
     * the backend has handed off every accepted frame, including our reply. */
    do {
        if (vm_net_stats(0, &stats) != 0 || stats.tx_failed || stats.backend_errors) return fail("backend failed while draining TX", 9);
    } while (stats.tx_sent != stats.tx_accepted);
    text("PING OK received="); number(received); text(" responded="); number(responded);
    text(" rejected="); number(rejected); text(" drops="); number(stats.rx_dropped); putchar('\n');
    text("NET counters rx_received="); number(stats.rx_received); text(" rx_delivered="); number(stats.rx_delivered);
    text(" rx_dropped="); number(stats.rx_dropped); text(" rx_oversized="); number(stats.rx_oversized);
    text(" tx_accepted="); number(stats.tx_accepted); text(" tx_sent="); number(stats.tx_sent);
    text(" tx_failed="); number(stats.tx_failed); text(" tx_would_block="); number(stats.tx_would_block);
    text(" backend_errors="); number(stats.backend_errors); text(" rx_queued="); number(stats.rx_queued);
    text(" tx_queued="); number(stats.tx_queued); putchar('\n');
    return 0;
}
