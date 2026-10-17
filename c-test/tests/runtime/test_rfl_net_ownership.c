/* Wrapper leases, bounded capacity, stale listener epochs and cancellation.
 * The NIC is deterministic; no host network setup or packet timing is needed. */
#include "net_tcp_test.h"
#include "../../../rfl/std/net/registry.c"
#include "../../../rfl/std/net/client.c"
#include "../../../rfl/std/net/dns.c"
#include "../../../rfl/std/net/server.c"
int main(void) {
    unsigned short config_words[18] = {0,7,192,168,64,42,255,255,255,0,192,168,64,1,1,1,1,1};
    unsigned short host[16] = {'1','9','2','.','1','6','8','.','6','4','.','1'};
    unsigned short path[2] = {'/'};
    unsigned long options[6] = {12,1,8080,30000UL,20000UL,123456UL};
    unsigned short status, i, words[16];
    unsigned short *handle, *driver, *client[4], *server, *old, *dns_jobs[4], *lease;
    unsigned long epoch;
    RflNet *owner;
    handle = rfl_net_create(config_words, 0, &status); owner = (RflNet *)handle;
    check(handle != 0 && status == NET_OK && owner->refs == 1);
    check(rfl_net_config(handle, words) == NET_OK && words[0] == 192 && words[15] == 1);
    check(rfl_http_begin(handle, host, path, options, &status) == 0 && status == NET_CANCELLED);
    driver = rfl_net_driver(handle);
    check(driver == handle && owner->refs == 2 && rfl_net_driver(handle) == 0);
    for (i = 0; i < 4; ++i) {
        client[i] = rfl_http_begin(handle, host, path, options, &status);
        check(client[i] != 0 && status == NET_OK);
    }
    check(rfl_http_begin(handle, host, path, options, &status) == 0 && status == NET_BUSY);
    rfl_http_drop(client[1]);
    client[1] = rfl_http_begin(handle, host, path, options, &status);
    check(client[1] != 0 && status == NET_OK && owner->refs == 6);
    for (i = 0; i < 4; ++i) rfl_http_drop(client[i]);
    check(owner->refs == 2 && rfl_net_pending_closes(handle) == 0);
    for (i = 0; i < 4; ++i) {
        dns_jobs[i] = rfl_dns_begin(handle, host, 12, 1000UL, &status);
        check(dns_jobs[i] != 0 && status == NET_OK);
    }
    check(rfl_dns_begin(handle, host, 12, 1000UL, &status) == 0 && status == NET_BUSY);
    rfl_dns_drop(dns_jobs[0]);
    dns_jobs[0] = rfl_dns_begin(handle, host, 12, 1000UL, &status);
    check(dns_jobs[0] != 0 && status == NET_OK && owner->refs == 6);
    for (i = 0; i < 4; ++i) rfl_dns_drop(dns_jobs[i]);
    check(owner->refs == 2);
    check(rfl_net_dhcp_begin(handle, 1000UL, &status) == 0 && status == NET_BAD_ARGUMENT);
    owner->refs = 0xffffffffUL;
    check(rfl_net_retain(handle) == 0 && owner->refs == 0xffffffffUL);
    owner->refs = 2;
    server = rfl_http_listen(handle, 8080, 123456UL, &status);
    check(server == handle && status == NET_OK && owner->refs == 3);
    /* Automatic error responses have no RFL IncomingRequest owner, but a
     * drain must still wait for their send and close phases. */
    owner->server->pool[0].accepted = 1;
    owner->server->sessions[0].state = NET_HTTP_SERVE_SENDING;
    check(rfl_net_pending_closes(handle) == 1);
    owner->server->sessions[0].state = NET_HTTP_SERVE_CLOSING;
    check(rfl_net_pending_closes(handle) == 1);
    owner->server->claimed[0] = 1;
    check(rfl_net_pending_closes(handle) == 0);
    owner->server->pool[0].accepted = 0; owner->server->sessions[0].state = 0; owner->server->claimed[0] = 0;
    epoch = rfl_http_listener_epoch(server); old = rfl_net_retain(handle);
    check(rfl_http_listen(handle, 8080, 123456UL, &status) == 0 && status == NET_BUSY);
    rfl_http_unlisten(server);
    check(rfl_http_accept(old, epoch, &status) == 0 && status == NET_CLOSED);
    server = rfl_http_listen(handle, 8080, 654321UL, &status);
    check(server == handle && status == NET_OK && rfl_http_listener_epoch(server) != epoch);
    check(rfl_http_accept(old, epoch, &status) == 0 && status == NET_CLOSED);
    rfl_net_release(old);
    rfl_net_driver_drop(driver);
    check(owner->stopped && owner->refs == 2 && rfl_net_pending_closes(handle) == 65535);
    check(rfl_http_accept(server, rfl_http_listener_epoch(server), &status) == 0 && status == NET_CANCELLED);
    rfl_net_release(handle); /* server lease keeps stopped storage alive */
    rfl_http_unlisten(server);
    handle = rfl_net_create(config_words, 1, &status); owner = (RflNet *)handle;
    driver = rfl_net_driver(handle);
    lease = rfl_net_dhcp_begin(handle, 30000UL, &status);
    check(lease != 0 && status == NET_OK && owner->refs == 3 && owner->dhcp.active);
    check(rfl_net_dhcp_begin(handle, 30000UL, &status) == 0 && status == NET_BUSY);
    rfl_net_dhcp_drop(lease);
    check(!owner->dhcp.active && !owner->dhcp_claim && owner->refs == 2);
    lease = rfl_net_dhcp_begin(handle, 30000UL, &status);
    check(lease != 0 && status == NET_OK);
    rfl_net_release(handle); rfl_net_driver_drop(driver);
    check(rfl_net_dhcp_status(lease) == NET_CANCELLED && owner->refs == 1);
    rfl_net_dhcp_drop(lease);
    putchar('\n'); return 0;
}
