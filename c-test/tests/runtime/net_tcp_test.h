#include <net_tcp.h>
#include <stdio.h>
#include <string.h>
static NetContext net;
static NetTcp tcp;
static NetConfig config = {0, {192,168,64,42}, {255,255,255,0}, {192,168,64,1}, {192,168,64,1}, 7};
static NetTcpConfig target = {{192,168,64,1}, 50000, 7777, 0xfffffff0UL, 60000UL};
static unsigned char incoming[1514], outgoing[1514], data[1460];
static unsigned short in_length, out_length, transmissions, connected = 1, blocked;
static unsigned long now, peer_next;
unsigned short vm_net_info(unsigned short id, VmNetInfo *info) {
    memset(info, 0, sizeof(*info)); info->link = connected; info->mtu = 1500; info->max_frame = 1514;
    info->mac[0] = 2; info->mac[5] = 1; return 0;
}
unsigned short vm_net_tx(unsigned short id, const unsigned char *p, unsigned short length, unsigned short *outcome) {
    if (!connected) { *outcome = VM_NET_LINK_DOWN; return 0; }
    if (blocked) { *outcome = VM_NET_WOULD_BLOCK; return 0; }
    *outcome = VM_NET_OK; memcpy(outgoing, p, length); out_length = length; ++transmissions; return 0;
}
unsigned short vm_net_rx(unsigned short id, unsigned char *p, unsigned short cap, VmNetReceive *result) {
    result->outcome = !connected ? VM_NET_LINK_DOWN : (in_length ? VM_NET_OK : VM_NET_EMPTY);
    result->length = connected ? in_length : 0;
    if (connected) memcpy(p, incoming, in_length);
    in_length = 0; return 0;
}
static void check(int ok) { putchar(ok ? 'Y' : 'N'); }
static unsigned short read16(const unsigned char *p) { return p[0] * 256U + p[1]; }
static void put16(unsigned char *p, unsigned short v) { p[0] = v / 256U; p[1] = v % 256U; }
static unsigned long read32(const unsigned char *p) { return (unsigned long)read16(p) * 65536UL + read16(p + 2); }
static void put32(unsigned char *p, unsigned long v) { put16(p, v / 65536UL); put16(p + 2, v % 65536UL); }
static unsigned long sum(const unsigned char *p, unsigned short n) {
    unsigned long value = 0;
    while (n >= 2) { value += read16(p); p += 2; n -= 2; }
    if (n) value += p[0] * 256UL;
    return value;
}
static unsigned short fold(unsigned long value) {
    while (value > 65535UL) value = (value & 65535UL) + value / 65536UL;
    return 65535UL - value;
}
static void fix_packet(void) {
    unsigned short length = in_length - 34;
    put16(incoming + 24, 0); put16(incoming + 24, fold(sum(incoming + 14, 20)));
    put16(incoming + 50, 0);
    put16(incoming + 50, fold(sum(incoming + 26, 8) + 6UL + length + sum(incoming + 34, length)));
}
static void packet(unsigned short flags, unsigned long sequence, unsigned long ack, unsigned short window, unsigned short length) {
    unsigned short header = (flags & 2) ? 24 : 20;
    memset(incoming, 0, sizeof(incoming)); memcpy(incoming, net.mac, 6); incoming[6] = 2; incoming[11] = 2;
    put16(incoming + 12, 0x0800); incoming[14] = 0x45; put16(incoming + 16, 20 + header + length);
    incoming[22] = 64; incoming[23] = 6; memcpy(incoming + 26, target.peer, 4); memcpy(incoming + 30, config.address, 4);
    put16(incoming + 34, target.remote_port); put16(incoming + 36, tcp.local_port);
    put32(incoming + 38, sequence); put32(incoming + 42, ack); incoming[46] = header * 4; incoming[47] = flags;
    put16(incoming + 48, window);
    if (flags & 2) { incoming[54] = 2; incoming[55] = 4; put16(incoming + 56, 128); }
    if (length) memcpy(incoming + 34 + header, data, length);
    in_length = 34 + header + length; fix_packet();
}
static void tick(unsigned long time) { now = time; net_poll(&net, now); net_tcp_poll(&tcp, now); }
static void route(void) {
    net.arp[0].used = 1; net.arp[0].ready = 1; net.arp[0].resolved = now;
    memcpy(net.arp[0].address, target.peer, 4); net.arp[0].mac[0] = 2; net.arp[0].mac[5] = 2;
}
static void reset(void) {
    unsigned short i;
    connected = 1; blocked = 0; in_length = 0; transmissions = 0; now = 0; peer_next = 0xfffffff8UL;
    memset(&tcp, 0, sizeof(tcp)); net_init(&net, &config); route();
    for (i = 0; i < 600; ++i) data[i] = i % 256;
}
static void start(void) { net_tcp_connect(&net, &tcp, &target, now); net_tcp_poll(&tcp, now); }
static void establish(void) {
    start(); packet(18, peer_next - 1, tcp.snd_nxt, 512, 0); tick(now + 10);
}
static int wire_ok(void) {
    unsigned short total = read16(outgoing + 16), length = total - 20;
    return !fold(sum(outgoing + 14, 20)) && !fold(sum(outgoing + 26, 8) + 6UL + length + sum(outgoing + 34, length));
}
