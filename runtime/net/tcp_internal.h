#ifndef RIPPLE_TCP_INTERNAL_H
#define RIPPLE_TCP_INTERNAL_H
#include "internal.h"
#include <net_tcp.h>
#define TCP_FIN 1
#define TCP_SYN 2
#define TCP_RST 4
#define TCP_PSH 8
#define TCP_ACK 16
#define TCP_URG 32
typedef struct TcpSegment {
    const unsigned char *source, *data;
    unsigned long sequence, acknowledgement;
    unsigned short source_port, destination_port, flags, window, length, mss;
} TcpSegment;
struct NetTcpListenDriver {
    int (*input)(NetContext *, const TcpSegment *);
};
int tcp_wait_save(NetTcp *tcp);
int tcp_wait_input(NetContext *net, const TcpSegment *segment);
int tcp_wait_port(NetContext *net, unsigned short port);
int tcp_passive_input(NetContext *net, const TcpSegment *segment);
void tcp_passive_receive(NetTcp *tcp, const TcpSegment *segment);
void tcp_established(NetTcp *tcp, const TcpSegment *segment);
int tcp_acceptable(NetTcp *tcp, const TcpSegment *segment);
int tcp_reset_segment(NetContext *net, const TcpSegment *segment);
unsigned long tcp_read32(const unsigned char *p);
void tcp_write32(unsigned char *p, unsigned long value);
int tcp_before(unsigned long a, unsigned long b);
int tcp_lifecycle(NetTcp *tcp);
int tcp_fail(NetTcp *tcp, int error);
void tcp_detach(NetTcp *tcp);
void tcp_forget(NetTcp *tcp);
void tcp_flight(NetTcp *tcp, unsigned short flags, unsigned short length);
int tcp_send(NetTcp *tcp, unsigned long sequence, unsigned short flags,
             const unsigned char *data, unsigned short length, unsigned short reliable);
int tcp_decode(NetContext *net, unsigned short header, unsigned short length, TcpSegment *segment);
void tcp_receive(NetTcp *tcp, const TcpSegment *segment);
int tcp_acknowledge(NetTcp *tcp, const TcpSegment *segment);
void tcp_payload(NetTcp *tcp, const TcpSegment *segment);
void tcp_time_wait(NetTcp *tcp);
void tcp_rtt(NetTcp *tcp);
void tcp_challenge(NetTcp *tcp);
#endif
