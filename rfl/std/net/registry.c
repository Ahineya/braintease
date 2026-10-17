#include "registry.h"

int rfl_net_keep(RflNet *n) {
    if (!n || n->refs == 0xffffffffUL) return RFL_NET_MEMORY;
    ++n->refs; return NET_OK;
}
unsigned short *rfl_net_retain(unsigned short *handle) {
    return rfl_net_keep((RflNet *)handle) ? 0 : handle;
}
void rfl_net_stop(unsigned short *handle) {
    RflNet *n = (RflNet *)handle;
    unsigned short i;
    if (!n || n->stopped) return;
    n->stopped = 1; if (n->server_drop) n->server_drop(n, 0);
    for (i = 0; i < RFL_NET_JOBS; ++i) if (n->jobs[i]) n->jobs[i]->cancel(n->jobs[i]);
    if (n->dns_stop) n->dns_stop(n);
    if (n->dhcp.active) net_dhcp_stop(&n->dhcp);
}
void rfl_net_release(unsigned short *handle) {
    RflNet *n = (RflNet *)handle;
    unsigned short i;
    if (!n || --n->refs) return;
    rfl_net_stop(handle);
    for (i = 0; i < RFL_NET_JOBS; ++i) if (n->jobs[i]) n->jobs[i]->destroy(n->jobs[i]);
    if (n->server_drop) n->server_drop(n, 1);
    free(n);
}
/* Config words: interface, seed, then four octets each of IP/mask/router/DNS.
 * No RFL object layout, string header or native address leaks into this ABI. */
unsigned short *rfl_net_create(unsigned short *words, unsigned short dynamic, unsigned short *status) {
    NetConfig config;
    RflNet *n;
    unsigned short i;
    *status = NET_BAD_ARGUMENT;
    if (!words || dynamic > 1) return 0;
    memset(&config, 0, sizeof(config)); config.interface = words[0]; config.seed = words[1];
    for (i = 0; i < 16; ++i) if (words[i + 2] > 255) return 0;
    for (i = 0; i < 4; ++i) {
        config.address[i] = words[2 + i]; config.mask[i] = words[6 + i];
        config.gateway[i] = words[10 + i]; config.dns[i] = words[14 + i];
    }
    n = calloc(1, sizeof(*n));
    if (!n) { *status = RFL_NET_MEMORY; return 0; }
    *status = dynamic ? net_open(&n->net, config.interface, config.seed) : net_init(&n->net, &config);
    if (*status) { free(n); return 0; }
    n->refs = 1; n->dynamic = dynamic; n->serial = ((unsigned long)config.seed << 16) | config.seed;
    *status = net_tcp_wait_attach(&n->net, &n->wait);
    if (*status) { free(n); return 0; }
    return (unsigned short *)n;
}
unsigned short *rfl_net_driver(unsigned short *handle) {
    RflNet *n = (RflNet *)handle;
    if (!n || n->driver || n->stopped || rfl_net_keep(n)) return 0;
    n->driver = 1; return handle;
}
void rfl_net_driver_drop(unsigned short *handle) {
    RflNet *n = (RflNet *)handle;
    if (!n) return;
    rfl_net_stop(handle); n->driver = 0; rfl_net_release(handle);
}
unsigned short rfl_net_service(unsigned short *handle, unsigned long now) {
    RflNet *n = (RflNet *)handle;
    unsigned short i;
    if (!n || n->stopped || !n->driver || n->pumping) return NET_CANCELLED;
    n->pumping = 1; n->now = now;
    for (i = 0; i < 4; ++i) {
        if (n->dhcp.active) net_dhcp_poll(&n->dhcp, now); else net_poll(&n->net, now);
    }
    if (n->client_service) n->client_service(n);
    if (n->server_service) n->server_service(n);
    if (n->dns_service) n->dns_service(n);
    n->pumping = 0; return NET_OK;
}
unsigned short rfl_net_config(unsigned short *handle, unsigned short *words) {
    RflNet *n = (RflNet *)handle;
    unsigned short i;
    if (!n || n->stopped) return NET_CANCELLED;
    if (!n->net.config.address[0]) return NET_AGAIN;
    for (i = 0; i < 4; ++i) {
        words[i] = n->net.config.address[i]; words[4+i] = n->net.config.mask[i];
        words[8+i] = n->net.config.gateway[i]; words[12+i] = n->net.config.dns[i];
    }
    return NET_OK;
}
unsigned long rfl_net_now(unsigned short *handle) { return ((RflNet *)handle)->now; }
unsigned short rfl_net_pending_closes(unsigned short *handle) {
    RflNet *n = (RflNet *)handle;
    unsigned short i, count = 0;
    if (!n || n->stopped) return 65535;
    for (i = 0; i < RFL_NET_JOBS; ++i) if (n->jobs[i] && n->jobs[i]->released) ++count;
    if (n->server) for (i = 0; i < NET_TCP_CONNECTIONS; ++i)
        if (n->server->pool[i].accepted && !n->server->claimed[i] &&
            (n->server->sessions[i].state == NET_HTTP_SERVE_SENDING ||
             n->server->sessions[i].state == NET_HTTP_SERVE_CLOSING ||
             n->server->sessions[i].state == NET_HTTP_SERVE_COMPLETE)) ++count;
    return count;
}

typedef struct RflDhcpJob { RflNet *owner; unsigned short done; } RflDhcpJob;
unsigned short *rfl_net_dhcp_begin(unsigned short *handle, unsigned long timeout, unsigned short *status) {
    RflNet *n = (RflNet *)handle;
    RflDhcpJob *job;
    *status = NET_BAD_ARGUMENT;
    if (!n || !n->dynamic || !timeout || timeout > 600000UL) return 0;
    if (n->stopped || !n->driver) { *status = NET_CANCELLED; return 0; }
    if (n->dhcp_claim) { *status = NET_BUSY; return 0; }
    job = calloc(1, sizeof(*job));
    if (!job) { *status = RFL_NET_MEMORY; return 0; }
    *status = rfl_net_keep(n);
    if (*status) { free(job); return 0; }
    if (!n->dhcp.active) *status = net_dhcp_start(&n->net, &n->dhcp, n->now, timeout);
    if (*status) { rfl_net_release(handle); free(job); return 0; }
    n->dhcp_claim = 1; job->owner = n; return (unsigned short *)job;
}
unsigned short rfl_net_dhcp_status(unsigned short *handle) {
    RflDhcpJob *job = (RflDhcpJob *)handle;
    RflNet *n = job->owner;
    if (n->stopped) return NET_CANCELLED;
    if (n->dhcp.state == NET_DHCP_FAILED) return n->dhcp.error ? n->dhcp.error : NET_IO_ERROR;
    if (n->dhcp.state == NET_DHCP_BOUND || n->dhcp.state == NET_DHCP_RENEWING || n->dhcp.state == NET_DHCP_REBINDING) {
        job->done = 1; return NET_OK;
    }
    return NET_AGAIN;
}
void rfl_net_dhcp_drop(unsigned short *handle) {
    RflDhcpJob *job = (RflDhcpJob *)handle;
    RflNet *n;
    if (!job) return;
    n = job->owner;
    if (!job->done && n->dhcp.active) net_dhcp_stop(&n->dhcp);
    n->dhcp_claim = 0; free(job); rfl_net_release((unsigned short *)n);
}
