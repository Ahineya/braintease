#include "demo.h"
#include <net_dhcp.h>
static NetContext net;
static NetDhcp lease;
int main(void) {
    VmClock clock;
    unsigned long acquired = 0, renewed = 0, expired = 0, link_epoch;
    if (net_open(&net, 0, 0x5246) || vm_get_time(&clock) || net_dhcp_start(&net, &lease, clock.ms, 60000UL)) return fail("DHCP startup failed", 1);
    puts("DHCP acquiring");
    link_epoch = net.link_epoch;
    for (;;) {
        net_dhcp_poll(&lease, clock.ms);
        if (lease.state == NET_DHCP_FAILED) return fail("DHCP failed", lease.error + 20);
        if (net.link_epoch != link_epoch) {
            link_epoch = net.link_epoch;
            puts(net.link ? "DHCP link restored; acquiring a fresh lease" : "DHCP link down; lease removed, waiting");
        }
        if (lease.expirations != expired) { expired = lease.expirations; puts("DHCP expired; address removed, reacquiring"); }
        if (lease.acquired != acquired || lease.renewals != renewed) {
            acquired = lease.acquired; renewed = lease.renewals;
            text("DHCP bound address="); ip_text(net.config.address); text(" mask="); ip_text(net.config.mask);
            text(" gateway="); ip_text(net.config.gateway); text(" dns="); ip_text(net.config.dns);
            text(" lease="); number(lease.lease); text(" renewals="); number(renewed); putchar('\n');
        }
        if (net.event == NET_EVENT_ECHO_REQUEST) {
            text("PING request from="); ip_text(net.peer); text(" seq="); number(net.sequence);
            text(" octets="); number(net.payload_length); putchar('\n'); puts("PING reply queued");
        }
        if (vm_next_tick(&clock)) return fail("clock tick failed", 2);
    }
}
