#ifndef NET_DEMO_H
#define NET_DEMO_H
#include <net.h>
#include <stdio.h>
#include <string.h>
#ifndef NET_LOCAL_IP
#define NET_LOCAL_IP 192,168,64,250
#endif
#ifndef NET_GATEWAY_IP
#define NET_GATEWAY_IP 192,168,64,1
#endif
#ifndef NET_MASK
#define NET_MASK 255,255,255,0
#endif
#ifndef NET_DNS_IP
#define NET_DNS_IP 1,1,1,1
#endif
static NetConfig config = {0, {NET_LOCAL_IP}, {NET_MASK}, {NET_GATEWAY_IP}, {NET_DNS_IP}, 0x5246};
/* Small logs don't need printf's floating-point formatter. */
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
static int fail(const char *message, int code) { text("PING ERROR: "); puts(message); return code; }
static void counters(const VmNetStats *s) {
    text("NET counters rx_received="); number(s->rx_received); text(" rx_delivered="); number(s->rx_delivered);
    text(" rx_dropped="); number(s->rx_dropped); text(" rx_oversized="); number(s->rx_oversized);
    text(" tx_accepted="); number(s->tx_accepted); text(" tx_sent="); number(s->tx_sent);
    text(" tx_failed="); number(s->tx_failed); text(" tx_would_block="); number(s->tx_would_block);
    text(" backend_errors="); number(s->backend_errors); text(" rx_queued="); number(s->rx_queued);
    text(" tx_queued="); number(s->tx_queued); putchar('\n');
}
#endif
