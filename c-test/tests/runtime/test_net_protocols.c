/* Guest stack with a deterministic fake NIC. No external networking. */
#include <net.h>
#include <stdio.h>
#include <string.h>
static NetContext net;
static unsigned char incoming[1514], outgoing[1514];
static unsigned short in_length, out_length, block_tx, calls, got_length, got_port;
static unsigned char got_data[4];
unsigned short vm_net_info(unsigned short interface, VmNetInfo *info) {
    memset(info, 0, sizeof(*info)); info->link = 1; info->mtu = 1500; info->max_frame = 1514;
    info->mac[0] = 2; info->mac[5] = 1; return 0;
}
unsigned short vm_net_tx(unsigned short interface, const unsigned char *frame, unsigned short length, unsigned short *outcome) {
    *outcome = block_tx ? VM_NET_WOULD_BLOCK : VM_NET_OK;
    if (!block_tx) { memcpy(outgoing, frame, length); out_length = length; }
    return 0;
}
unsigned short vm_net_rx(unsigned short interface, unsigned char *frame, unsigned short capacity, VmNetReceive *result) {
    result->outcome = in_length ? VM_NET_OK : VM_NET_EMPTY; result->length = in_length;
    if (in_length) memcpy(frame, incoming, in_length);
    in_length = 0; return 0;
}
static void check(int condition) { putchar(condition ? 'Y' : 'N'); }
static unsigned short read16(const unsigned char *p) { return p[0] * 256U + p[1]; }
static void write16(unsigned char *p, unsigned short n) { p[0] = n / 256U; p[1] = n % 256U; }
/* Independent wide accumulator, unlike the stack's carry-per-word checksum. */
static unsigned short checksum(const unsigned char *p, unsigned short n) {
    unsigned long sum = 0;
    while (n >= 2) { sum += read16(p); p += 2; n -= 2; }
    if (n) sum += p[0] * 256UL;
    while (sum > 65535UL) sum = (sum & 65535UL) + sum / 65536UL;
    return 65535UL - sum;
}
static void receive(NetContext *ctx, const NetDatagram *packet, void *user) {
    unsigned short *counter = user;
    ++*counter; got_length = packet->length; got_port = packet->source_port;
    if (packet->length <= 4) memcpy(got_data, packet->data, packet->length);
}
static void datagram(unsigned short size, unsigned short udp_size, unsigned short with_checksum) {
    unsigned char pseudo[24];
    unsigned short i;
    memset(incoming, 0, 60); incoming[0] = 2; incoming[5] = 1;
    incoming[6] = 2; incoming[11] = 2; incoming[12] = 8;
    incoming[14] = 0x45; write16(incoming + 16, 28 + size); incoming[22] = 64; incoming[23] = 17;
    incoming[26] = 192; incoming[27] = 168; incoming[28] = 64; incoming[29] = 1;
    memcpy(incoming + 30, net.config.address, 4); write16(incoming + 24, checksum(incoming + 14, 20));
    write16(incoming + 34, 1234); write16(incoming + 36, 9000); write16(incoming + 38, udp_size);
    for (i = 0; i < size; ++i) incoming[42 + i] = i == 1 ? 255 : i;
    if (with_checksum) {
        memcpy(pseudo, incoming + 26, 8); pseudo[8] = 0; pseudo[9] = 17; write16(pseudo + 10, 8 + size);
        memcpy(pseudo + 12, incoming + 34, 8 + size);
        write16(incoming + 40, checksum(pseudo, 20 + size));
    }
    in_length = 42 + size;
}
int main(void) {
    NetConfig config = {0, {192,168,64,250}, {255,255,255,0}, {192,168,64,1}, {1,1,1,1}, 7};
    unsigned char data[3] = {0,255,2}, address[4] = {192,168,64,1}, pseudo[23];
    unsigned short before;
    check(net_init(&net, &config) == NET_OK && out_length == 60 && read16(outgoing + 20) == 1);
    check(net_udp_bind(&net, 0, receive, &calls) == NET_BAD_ARGUMENT && net_udp_bind(&net, 9000, receive, &calls) == NET_OK);
    check(net_udp_bind(&net, 9000, receive, &calls) == NET_NO_PORT && net_udp_bind(&net, 9001, 0, &calls) == NET_BAD_ARGUMENT);
    check(net_udp_bind(&net, 9001, receive, &calls) == NET_OK && net_udp_bind(&net, 9002, receive, &calls) == NET_OK &&
          net_udp_bind(&net, 9003, receive, &calls) == NET_OK && net_udp_bind(&net, 9004, receive, &calls) == NET_NO_PORT);
    net.arp[0].used = 1; net.arp[0].ready = 1; memcpy(net.arp[0].address, address, 4); net.arp[0].mac[0] = 2; net.arp[0].mac[5] = 2;
    check(net_udp_send(&net, address, 9000, 53, data, 3) == NET_OK && read16(outgoing + 38) == 11 && !memcmp(outgoing + 42, data, 3));
    memcpy(pseudo, outgoing + 26, 8); pseudo[8] = 0; pseudo[9] = 17; write16(pseudo + 10, 11); memcpy(pseudo + 12, outgoing + 34, 11);
    check(!checksum(pseudo, 23) && !checksum(outgoing + 14, 20) && read16(outgoing + 40) != 0);
    check(net_udp_send(&net, address, 9000, 53, 0, 0) == NET_OK && read16(outgoing + 38) == 8);
    check(net_udp_send(&net, address, 9000, 0, data, 3) == NET_BAD_ARGUMENT && net_udp_send(&net, address, 9000, 53, data, 1473) == NET_BAD_ARGUMENT);
    block_tx = 1;
    check(net_udp_send(&net, address, 9000, 53, data, 3) == NET_OK && net.pending == 60 && net_udp_send(&net, address, 9000, 53, data, 3) == NET_AGAIN);
    block_tx = 0; check(net_poll(&net, 10) == NET_AGAIN && !net.pending && !memcmp(outgoing + 42, data, 3));
    datagram(3, 11, 1); check(net_poll(&net, 20) == NET_OK && calls == 1 && got_length == 3 && got_port == 1234 && !memcmp(got_data, data, 3));
    datagram(0, 8, 0); check(net_poll(&net, 30) == NET_OK && calls == 2 && got_length == 0);
    before = calls; datagram(3, 11, 1); incoming[40] ^= 1;
    check(net_poll(&net, 40) == NET_REJECTED && calls == before);
    datagram(3, 7, 0); check(net_poll(&net, 50) == NET_REJECTED && calls == before);
    datagram(3, 12, 0); check(net_poll(&net, 60) == NET_REJECTED && calls == before);
    datagram(3, 11, 0); write16(incoming + 36, 9999);
    check(net_poll(&net, 70) == NET_OK && calls == before && net.rejected == 3);
    check(net_udp_unbind(&net, 9000) == NET_OK && net_udp_unbind(&net, 9000) == NET_NO_PORT);
    datagram(3, 11, 1); check(net_poll(&net, 80) == NET_OK && calls == before);
    putchar('\n'); return 0;
}
