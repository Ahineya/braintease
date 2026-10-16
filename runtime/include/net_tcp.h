#ifndef RIPPLE_NET_TCP_H
#define RIPPLE_NET_TCP_H
#include <net.h>
#define NET_TCP_BUFFER 512
#define NET_TCP_MSS 512
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

typedef struct NetTcpConfig {
    unsigned char peer[4];
    unsigned short local_port, remote_port;
    /* Supply an unpredictable ISN for untrusted networks. The library does not
     * replace caller entropy with a fixed or supposedly secure PRNG seed. */
    unsigned long initial_sequence, timeout_ms;
} NetTcpConfig;
/* Caller-owned: zero before first use; never move/free an active connection.
 * One reliable segment in flight; all queued stream data is copied here. */
struct NetTcp {
    NetContext *net;
    unsigned char peer[4], tx[NET_TCP_BUFFER], rx[NET_TCP_BUFFER];
    unsigned short local_port, remote_port, state, error, active;
    unsigned short tx_used, rx_used, rx_head, mss, window;
    unsigned short shutting, peer_fin, ack_pending;
    unsigned short inflight, flight_data, flight_flags, flight_sent;
    unsigned short retransmitted, sample_valid, retries, queued, queued_flight;
    unsigned long snd_una, snd_nxt, sent_nxt, rcv_nxt, flight_seq, window_seq, window_ack;
    unsigned long now, generation, link_epoch, timeout, last_progress, phase_at;
    unsigned long sent_at, sample_at, rto, base_rto, srtt, rttvar;
    unsigned long retransmits, probes, bytes_written, bytes_acked, bytes_received, bytes_read;
    unsigned long challenge_at;
    unsigned long zero_at, zero_delay;
    unsigned short challenge_sent, icmp_type, icmp_code, mtu;
};
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
/* Drain accepted writes, then send FIN. Reading remains available. */
int net_tcp_shutdown(NetTcp *tcp);
/* Explicit cancellation: best-effort RST, discard buffers and release the slot.
 * Aborting TIME_WAIT deliberately gives up its tuple-reuse protection. */
void net_tcp_abort(NetTcp *tcp);
#endif
