#ifndef RIPPLE_NET_TCP_H
#define RIPPLE_NET_TCP_H
#include <net.h>
#define NET_TCP_BUFFER 512
#ifdef RIPPLE_FUZIX_NETWORK
/* Backlog/idle TCBs stay compact; active sockets may attach larger rings.
 * This also leaves room for pipes/IPC on small-memory VM targets. */
#define NET_TCP_RX_BUFFER 1024
#define NET_TCP_MSS 1460
#else
#define NET_TCP_RX_BUFFER NET_TCP_BUFFER
#define NET_TCP_MSS 512
#endif
#define NET_TCP_TIME_WAIT_MS 120000UL
#define NET_TCP_CLOSED 0
#define NET_TCP_SYN_SENT 1
#define NET_TCP_ESTABLISHED 2
#define NET_TCP_FIN_WAIT_1 3
#define NET_TCP_FIN_WAIT_2 4
#define NET_TCP_CLOSE_WAIT 5
#define NET_TCP_CLOSING 6
#define NET_TCP_LAST_ACK 7
#define NET_TCP_TIME_WAIT 8
#define NET_TCP_FAILED 9
#define NET_TCP_SYN_RECEIVED 10

typedef struct NetTcpConfig {
    unsigned char peer[4];
    unsigned short local_port, remote_port;
    /* Supply an unpredictable ISN for untrusted networks. The library does not
     * replace caller entropy with a fixed or supposedly secure PRNG seed. */
    unsigned long initial_sequence, timeout_ms;
} NetTcpConfig;
/* Caller-owned: zero before first use; never move/free an active connection.
 * Inline buffers retain the compact stop-and-wait profile. Optional external
 * buffers enable bounded windowed streams; storage stays caller-owned. */
struct NetTcp {
    NetContext *net;
    unsigned char peer[4], tx[NET_TCP_BUFFER], rx[NET_TCP_RX_BUFFER];
    unsigned short local_port, remote_port, state, error, active;
    unsigned short tx_used, rx_used, rx_head, mss, window;
    unsigned short shutting, peer_fin, ack_pending;
    unsigned short inflight, flight_data, flight_flags, flight_sent;
    unsigned short retransmitted, sample_valid, retries, queued, queued_flight;
    unsigned long snd_una, snd_nxt, sent_nxt, rcv_nxt, flight_seq, window_seq, window_ack;
    unsigned long now, generation, link_epoch, timeout, last_progress, phase_at;
    unsigned long sent_at, sample_at, sample_end, rto, base_rto, srtt, rttvar;
    unsigned long retransmits, probes, bytes_written, bytes_acked, bytes_received, bytes_read;
    unsigned long challenge_at;
    unsigned long zero_at, zero_delay;
    unsigned short challenge_sent, icmp_type, icmp_code, mtu;
    unsigned short accepted;
    unsigned char *send_buffer, *receive_buffer, *receive_map;
    unsigned short send_capacity, receive_capacity;
    unsigned short cwnd, ssthresh, duplicate_acks, recovering;
    unsigned long recovery_end;
    unsigned short pending_fin, reorder_used;
    unsigned long fin_sequence;
};
/* Optional caller-owned compact TIME_WAIT storage. Zero before first use and
 * keep attached storage stable for the context lifetime or explicit detach.
 * Without it, release returns BUSY until the full TCB's TIME_WAIT expires. */
typedef struct NetTcpWait {
    unsigned char peer[4];
    unsigned short local_port, remote_port, used;
    unsigned long snd_nxt, rcv_nxt, started;
} NetTcpWait;
struct NetTcpWaitTable {
    NetContext *net;
    NetTcpWait entries[NET_TCP_WAIT_SLOTS];
    unsigned short used;
    unsigned long generation, link_epoch;
};
/* NULL detaches only an empty/expired table. Never evicts a live tuple. */
int net_tcp_wait_attach(NetContext *net, NetTcpWaitTable *table);
/* Relinquish a finished active-open TCB without losing TIME_WAIT protection.
 * OK means inactive/storage may be freed. BUSY keeps the TCB live and must be
 * retried after polling (including when the compact wait table is full).
 * Accepted objects still require net_tcp_release to return their pool slot. */
int net_tcp_retire(NetTcp *tcp);

/* Called synchronously for each new passive open. Supply fresh, unpredictable
 * ISNs for untrusted networks. Do not recursively poll or mutate the context. */
typedef unsigned long (*NetTcpSequence)(void *, const unsigned char *, unsigned short, unsigned long);
typedef struct NetTcpListenConfig {
    unsigned short port;
    unsigned long timeout_ms;
    NetTcpSequence sequence;
    void *user;
} NetTcpListenConfig;
/* Zero the listener AND its exclusive pool before first use. Do not use pool
 * objects for active connects/another context while reserved. Storage stays stable
 * until unlisten and all accepted connections have been released. */
struct NetTcpListener {
    NetContext *net;
    NetTcp *pool;
    NetTcpSequence sequence;
    void *user;
    unsigned short port, capacity, active, error;
    unsigned long timeout, generation, link_epoch, accepted, refused;
};
int net_tcp_listen(NetContext *net, NetTcpListener *listener, NetTcp *pool,
    unsigned short capacity, const NetTcpListenConfig *config);
/* Drive every pool connection, including closing/TIME_WAIT and unaccepted
 * handshakes. Network/DHCP poll still comes first. */
int net_tcp_listener_poll(NetTcpListener *listener, unsigned long now_ms);
/* Returns a borrowed pool object after handshake; data/FIN can already be queued.
 * Keep it until explicitly released, including after failure/close. */
int net_tcp_accept(NetTcpListener *listener, NetTcp **connection);
/* Release inactive accepted objects, or move TIME_WAIT into a compact context
 * record. Other active states/full record table return NET_BUSY. Old tuples stay
 * protected; no buffer is reused until release succeeds. */
int net_tcp_release(NetTcpListener *listener, NetTcp *connection);
/* Stop new opens and abort unaccepted work. Accepted connections remain alive
 * and owned by the application: continue polling/release them. */
void net_tcp_unlisten(NetTcpListener *listener);

/* Optional: enable closed-port RST handling without opening a connection.
 * connect enables the protocol automatically; other applications link no TCP. */
void net_tcp_enable(NetContext *net);
/* Active open only. local_port=0 chooses an ephemeral port. timeout_ms is
 * 1..600000 for handshake/idle progress. OK starts, rather than completes, open. */
int net_tcp_connect(NetContext *net, NetTcp *tcp, const NetTcpConfig *config, unsigned long now_ms);
/* Call net_poll or net_dhcp_poll first, then this for every active connection.
 * Poll drives transmissions and timers; it does not receive NIC frames. */
int net_tcp_poll(NetTcp *tcp, unsigned long now_ms);
int net_tcp_write(NetTcp *tcp, const unsigned char *data, unsigned short length, unsigned short *written);
int net_tcp_read(NetTcp *tcp, unsigned char *data, unsigned short capacity, unsigned short *received);
/* Upgrade immediately after connect (before polling) or after accept. No queued
 * TX is permitted; unread RX is preserved. Buffers must be disjoint, stable and
 * caller-owned until release/retire. RX map has (receive_capacity+7)/8 cells.
 * Capacities: TX 512..16384, RX current-used..32768 (minimum inline capacity).
 * Allocation policy belongs to the caller; there are no hidden allocations. */
int net_tcp_buffers(NetTcp *, unsigned char *send, unsigned short send_capacity,
    unsigned char *receive, unsigned short receive_capacity, unsigned char *map);
#define NET_TCP_TX_DATA(t) ((t)->send_buffer ? (t)->send_buffer : (t)->tx)
#define NET_TCP_RX_DATA(t) ((t)->receive_buffer ? (t)->receive_buffer : (t)->rx)
#define NET_TCP_TX_CAP(t) ((t)->send_buffer ? (t)->send_capacity : NET_TCP_BUFFER)
#define NET_TCP_RX_CAP(t) ((t)->receive_buffer ? (t)->receive_capacity : NET_TCP_RX_BUFFER)
/* Drain accepted writes, then send FIN. Reading remains available. */
int net_tcp_shutdown(NetTcp *tcp);
/* Explicit cancellation: best-effort RST, discard buffers and release the slot.
 * Aborting TIME_WAIT deliberately gives up its tuple-reuse protection. */
void net_tcp_abort(NetTcp *tcp);
#endif
