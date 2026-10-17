#include "registry.h"
static void service(RflNet *n) {
    unsigned short i;
    for (i = 0; i < RFL_NET_JOBS; ++i) if (n->dns_jobs[i]) net_dns_poll(&n->dns_jobs[i]->dns, n->now);
}
static void stop(RflNet *n) {
    unsigned short i;
    for (i = 0; i < RFL_NET_JOBS; ++i) if (n->dns_jobs[i]) net_dns_cancel(&n->dns_jobs[i]->dns);
}
unsigned short *rfl_dns_begin(unsigned short *handle, unsigned short *name, unsigned short length,
    unsigned long timeout, unsigned short *status) {
    RflNet *n = (RflNet *)handle;
    RflDnsJob *job;
    char text[254];
    unsigned short i, slot;
    *status = NET_BAD_ARGUMENT;
    if (!n || !name || !length || length > 253) return 0;
    if (n->stopped || !n->driver) { *status = NET_CANCELLED; return 0; }
    for (i = 0; i < length; ++i) { if (!name[i] || name[i] > 127) return 0; text[i] = name[i]; }
    text[length] = 0;
    for (slot = 0; slot < RFL_NET_JOBS && n->dns_jobs[slot]; ++slot) {}
    if (slot == RFL_NET_JOBS) { *status = NET_BUSY; return 0; }
    job = calloc(1, sizeof(*job)); if (!job) { *status = RFL_NET_MEMORY; return 0; }
    *status = net_dns_start(&n->net, &job->dns, text, n->now, timeout, 2);
    if (*status) { free(job); return 0; }
    *status = rfl_net_keep(n);
    if (*status) { net_dns_cancel(&job->dns); free(job); return 0; }
    n->dns_service = service; n->dns_stop = stop; job->owner = n; job->slot = slot; n->dns_jobs[slot] = job; return (unsigned short *)job;
}
unsigned short rfl_dns_status(unsigned short *handle, unsigned short *address) {
    RflDnsJob *job = (RflDnsJob *)handle;
    unsigned short i;
    if (job->owner->stopped) return NET_DNS_CANCELLED;
    if (job->dns.status == NET_DNS_SUCCESS) for (i = 0; i < 4; ++i) address[i] = job->dns.address[i];
    return job->dns.status;
}
void rfl_dns_drop(unsigned short *handle) {
    RflDnsJob *job = (RflDnsJob *)handle;
    RflNet *n;
    if (!job) return;
    n = job->owner; net_dns_cancel(&job->dns); n->dns_jobs[job->slot] = 0;
    free(job); rfl_net_release((unsigned short *)n);
}
