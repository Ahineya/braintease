#include "registry.h"

static void dispose(RflHttpJob *job) {
    job->owner->jobs[job->slot] = 0; free(job);
}
static void cancel(RflHttpJob *job) { net_http_cancel(&job->http); }
void rfl_http_destroy(RflHttpJob *job) { net_http_cancel(&job->http); dispose(job); }
void rfl_http_service(RflNet *n) {
    RflHttpJob *job;
    unsigned short i;
    for (i = 0; i < RFL_NET_JOBS; ++i) {
        job = n->jobs[i]; if (!job) continue;
        net_http_poll(&job->http, n->now);
        if (job->released && net_tcp_retire(&job->http.tcp) == NET_OK) dispose(job);
    }
}
/* Options are u32 words: host length, path length, port, timeout, body limit, ISN.
 * A compact boundary also avoids tests/bugs/test_call_many_u32_arguments. */
unsigned short *rfl_http_begin(unsigned short *handle, unsigned short *host, unsigned short *path,
    unsigned long *options, unsigned short *status) {
    RflNet *n = (RflNet *)handle;
    RflHttpJob *job;
    NetHttpConfig config;
    char hostname[254], target[256];
    unsigned short i, slot, host_length, path_length;
    *status = NET_BAD_ARGUMENT;
    if (!n || !host || !path || !options || !options[0] || options[0] > 253 || !options[1] || options[1] > 255 || !options[2] || options[2] > 65535UL || options[4] > RFL_NET_BODY_LIMIT) return 0;
    host_length = options[0]; path_length = options[1];
    if (n->stopped || !n->driver) { *status = NET_CANCELLED; return 0; }
    for (i = 0; i < host_length; ++i) { if (!host[i] || host[i] > 127) return 0; hostname[i] = host[i]; }
    for (i = 0; i < path_length; ++i) { if (!path[i] || path[i] > 127) return 0; target[i] = path[i]; }
    hostname[host_length] = 0; target[path_length] = 0;
    for (slot = 0; slot < RFL_NET_JOBS && n->jobs[slot]; ++slot) {}
    if (slot == RFL_NET_JOBS) { *status = NET_BUSY; return 0; }
    job = calloc(1, sizeof(*job));
    if (!job) { *status = RFL_NET_MEMORY; return 0; }
    config.host = hostname; config.path = target; config.port = options[2];
    config.timeout_ms = options[3]; config.max_body = options[4]; config.initial_sequence = options[5];
    *status = net_http_start(&n->net, &job->http, &config, n->now);
    if (*status) { free(job); return 0; }
    *status = rfl_net_keep(n);
    if (*status) { net_http_cancel(&job->http); free(job); return 0; }
    job->owner = n; job->slot = slot; job->cancel = cancel; job->destroy = rfl_http_destroy; n->client_service = rfl_http_service; n->jobs[slot] = job; return (unsigned short *)job;
}
unsigned short rfl_http_read(unsigned short *handle, unsigned short *output, unsigned short *count) {
    RflHttpJob *job = (RflHttpJob *)handle;
    unsigned char bytes[256];
    unsigned short i;
    int status;
    *count = 0;
    if (!job || job->released || job->owner->stopped) return NET_CANCELLED;
    status = net_http_read(&job->http, bytes, 256, count);
    for (i = 0; i < *count; ++i) output[i] = bytes[i];
    return status;
}
unsigned short rfl_http_status(unsigned short *handle) { return ((RflHttpJob *)handle)->http.response.status; }
unsigned short rfl_http_dns_status(unsigned short *handle) { return ((RflHttpJob *)handle)->http.dns.status; }
void rfl_http_drop(unsigned short *handle) {
    RflHttpJob *job = (RflHttpJob *)handle;
    RflNet *n;
    if (!job) return;
    n = job->owner;
    if (job->http.state == NET_HTTP_COMPLETE) {
        job->released = 1;
        if (net_tcp_retire(&job->http.tcp) == NET_OK) dispose(job);
    } else rfl_http_destroy(job);
    rfl_net_release((unsigned short *)n);
}
