#include "net_tcp_test.h"
static NetTcpListener listener, other;
static NetTcp pool[2], spare[1];
static NetTcpWaitTable closing, alternate;
static unsigned long isn_calls;
static unsigned long sequence(void *user, const unsigned char *peer, unsigned short port, unsigned long time) {
    unsigned long *calls = user; ++*calls; return 0xfffffff0UL + *calls - 1;
}
static NetTcpListenConfig listening = {8080, 5000UL, sequence, &isn_calls};
static void prepare(void) {
    net_tcp_unlisten(&listener); net_tcp_unlisten(&other);
    memset(&listener, 0, sizeof(listener)); memset(&other, 0, sizeof(other));
    memset(pool, 0, sizeof(pool)); memset(spare, 0, sizeof(spare)); isn_calls = 0;
    reset(); memset(&closing, 0, sizeof(closing)); net_tcp_wait_attach(&net, &closing); tcp.local_port = 8080;
}
static void listen_now(void) { net_tcp_listen(&net, &listener, pool, 2, &listening); }
static void poll(unsigned long time) { now = time; net_poll(&net, now); net_tcp_listener_poll(&listener, now); }
static void syn(unsigned short port) {
    packet(2, peer_next - 1, 0, 512, 0); put16(incoming + 34, port); fix_packet(); poll(now + 1);
}
static void final_ack(unsigned short flags, unsigned short size) {
    packet(flags, peer_next, pool[0].snd_nxt, 512, size); poll(now + 1);
}
int main(void) {
    NetTcp *accepted = 0;
    unsigned short count, before, i;
    prepare();
    check(net_tcp_wait_attach(&net, &closing) == NET_OK);
    check(net_tcp_wait_attach(&net, 0) == NET_OK && !net.tcp_wait && !closing.net);
    net_tcp_wait_attach(&net, &closing); memset(&alternate, 0, sizeof(alternate)); alternate.used = 1;
    check(net_tcp_wait_attach(&net, &alternate) == NET_BUSY && net.tcp_wait == &closing);
    check(net_tcp_listen(&net, &listener, pool, 0, &listening) == NET_BAD_ARGUMENT);
    check(net_tcp_listen(&net, &listener, pool, 2, &listening) == NET_OK && listener.active);
    check(net_tcp_listen(&net, &listener, pool, 2, &listening) == NET_BUSY);
    check(net_tcp_listen(&net, &other, spare, 1, &listening) == NET_NO_PORT);
    listening.port = 8081;
    check(net_tcp_listen(&net, &other, pool, 1, &listening) == NET_BUSY); listening.port = 8080;
    target.local_port = 8080;
    check(net_tcp_connect(&net, &tcp, &target, 0) == NET_NO_PORT); target.local_port = 50000;
    check(net_tcp_accept(&listener, &accepted) == NET_AGAIN && !accepted);
    syn(7777);
    check(pool[0].state == NET_TCP_SYN_RECEIVED && pool[0].mss == 128 && isn_calls == 1 && net.tcp[0] == pool);
    check(outgoing[47] == 18 && read16(outgoing + 34) == 8080 && read32(outgoing + 38) == 0xfffffff0UL && read32(outgoing + 42) == peer_next && wire_ok());
    check(net_tcp_write(pool, data, 1, &count) == NET_AGAIN && !count);
    check(net_tcp_accept(&listener, &accepted) == NET_AGAIN);
    before = transmissions; poll(1001);
    check(transmissions > before && pool[0].retransmits == 1 && outgoing[47] == 18);
    syn(7777); check(isn_calls == 1 && pool[0].active && pool[0].state == NET_TCP_SYN_RECEIVED);
    packet(16, peer_next, pool[0].snd_nxt + 1, 512, 0); poll(now + 1);
    check(pool[0].state == NET_TCP_SYN_RECEIVED && outgoing[47] == 4);
    final_ack(24, 3);
    check(pool[0].state == NET_TCP_ESTABLISHED && pool[0].rx_used == 3 && pool[0].tx_used == 0 && pool[0].snd_una == 0xfffffff1UL);
    check(net_tcp_accept(&listener, &accepted) == NET_OK && accepted == pool && pool[0].accepted && listener.accepted == 1);
    check(net_tcp_accept(&listener, &accepted) == NET_AGAIN && !accepted);
    check(net_tcp_read(pool, data + 20, 10, &count) == NET_OK && count == 3 && data[20] == 0 && data[22] == 2);
    check(net_tcp_release(&listener, pool) == NET_BUSY);
    syn(7778); check(pool[1].active && isn_calls == 2);
    syn(7779); check(listener.refused == 1 && isn_calls == 2 && !net.tcp[2]);
    net_tcp_unlisten(&listener); check(!listener.active && pool[0].active && !pool[1].active && pool[1].error == NET_CANCELLED);
    check(net_tcp_listen(&net, &listener, spare, 1, &listening) == NET_BUSY);
    check(net_tcp_listener_poll(&listener, now + 1) == NET_CLOSED && pool[0].active);
    net_tcp_abort(pool); check(net_tcp_release(&listener, pool) == NET_OK && !pool[0].accepted);
    check(net_tcp_release(&listener, pool) == NET_BAD_ARGUMENT);
    check(net_tcp_listen(&net, &listener, pool, 2, &listening) == NET_OK);
    syn(7777); poll(now + 5000UL); check(!pool[0].active && pool[0].error == NET_TIMEOUT && listener.active);
    syn(7777); final_ack(17, 0);
    check(pool[0].state == NET_TCP_CLOSE_WAIT && pool[0].peer_fin && net_tcp_accept(&listener, &accepted) == NET_OK);
    check(net_tcp_read(pool, data, 1, &count) == NET_EOF);
    net_tcp_shutdown(pool); poll(now + 1); check(pool[0].state == NET_TCP_LAST_ACK);
    packet(16, peer_next + 1, pool[0].snd_nxt, 512, 0); poll(now + 1);
    check(!pool[0].active && pool[0].state == NET_TCP_CLOSED && pool[0].accepted);
    check(net_tcp_release(&listener, pool) == NET_OK);
    syn(7777); packet(4, peer_next, 0, 0, 0); poll(now + 1); check(!pool[0].active && pool[0].error == NET_RESET);
    syn(7777); final_ack(16, 0); net_tcp_accept(&listener, &accepted);
    config.dns[3] = 2; net_configure(&net, &config); config.dns[3] = 1;
    check(net_tcp_listener_poll(&listener, now + 1) == NET_CONFIG_CHANGED && !listener.active && !pool[0].active && pool[0].error == NET_CONFIG_CHANGED);
    check(net_tcp_release(&listener, pool) == NET_OK);
    prepare(); listen_now(); syn(7777); connected = 0; poll(now + 1);
    check(!listener.active && listener.error == NET_LINK_DOWN && !pool[0].active);
    prepare(); listen_now(); blocked = 1; syn(7777);
    check(pool[0].queued && !pool[0].flight_sent);
    final_ack(16, 0); check(pool[0].state == NET_TCP_SYN_RECEIVED && !pool[0].accepted);
    blocked = 0; poll(now + 1); final_ack(16, 0);
    check(pool[0].state == NET_TCP_ESTABLISHED && net_tcp_accept(&listener, &accepted) == NET_OK);
    net_tcp_shutdown(pool); poll(now + 1); final_ack(17, 0);
    for (i = 0; i < NET_TCP_WAIT_SLOTS; ++i) { closing.entries[i].used = 1; closing.entries[i].started = now; }
    closing.used = NET_TCP_WAIT_SLOTS;
    check(net_tcp_release(&listener, pool) == NET_BUSY && pool[0].active);
    memset(closing.entries, 0, sizeof(closing.entries)); closing.used = 0;
    check(pool[0].state == NET_TCP_TIME_WAIT && net_tcp_release(&listener, pool) == NET_OK && closing.entries[0].used);
    check(net_tcp_wait_attach(&net, 0) == NET_BUSY);
    packet(17, peer_next, closing.entries[0].snd_nxt, 512, 0); poll(now + 100);
    check(closing.entries[0].started == now && outgoing[47] == 16 && read32(outgoing + 42) == peer_next + 1 && wire_ok());
    net_tcp_unlisten(&listener); target.local_port = 8080;
    check(net_tcp_connect(&net, &tcp, &target, now) == NET_NO_PORT); target.local_port = 50000;
    listen_now();
    before = isn_calls; syn(7777); check(isn_calls == before && !pool[0].active);
    poll(now + NET_TCP_TIME_WAIT_MS); syn(7777); check(pool[0].active && isn_calls == before + 1);
    check(net_tcp_wait_attach(&net, 0) == NET_OK && !net.tcp_wait);
    prepare(); listen_now(); packet(2, peer_next - 1, 0, 512, 1); poll(1);
    check(listener.refused == 1 && !pool[0].active);
    putchar('\n'); return 0;
}
