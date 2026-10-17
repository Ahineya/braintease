#ifndef RFL_NET_REGISTRY_H
#define RFL_NET_REGISTRY_H
#include <net_dhcp.h>
#include <net_http_server.h>
#include <stdlib.h>
#include <string.h>
#define RFL_NET_MEMORY 64
#define RFL_NET_JOBS 4
#define RFL_NET_BODY_LIMIT 1048576UL
typedef struct RflHttpJob RflHttpJob;
typedef struct RflDnsJob RflDnsJob;
typedef struct RflNet {
    unsigned long refs, now, serial;
    unsigned short driver, stopped, pumping, dynamic, dhcp_claim;
    NetContext net;
    NetDhcp dhcp;
    NetTcpWaitTable wait;
    RflHttpJob *jobs[RFL_NET_JOBS];
    RflDnsJob *dns_jobs[RFL_NET_JOBS];
    struct RflServer *server;
    void (*client_service)(struct RflNet *);
    void (*dns_service)(struct RflNet *);
    void (*dns_stop)(struct RflNet *);
    void (*server_service)(struct RflNet *);
    void (*server_drop)(struct RflNet *, unsigned short);
} RflNet;
typedef struct RflServer {
    NetTcpListener listener;
    NetTcp pool[NET_TCP_CONNECTIONS];
    NetHttpServer sessions[NET_TCP_CONNECTIONS];
    unsigned char *bodies[NET_TCP_CONNECTIONS];
    unsigned long generations[NET_TCP_CONNECTIONS], copied[NET_TCP_CONNECTIONS];
    unsigned short claimed[NET_TCP_CONNECTIONS], accepting;
    unsigned long epoch;
} RflServer;
struct RflHttpJob {
    RflNet *owner;
    NetHttp http;
    unsigned short slot, released;
    void (*cancel)(struct RflHttpJob *);
    void (*destroy)(struct RflHttpJob *);
};
typedef struct RflIncoming {
    RflNet *owner;
    unsigned short slot;
    unsigned long generation;
} RflIncoming;
struct RflDnsJob { RflNet *owner; NetDns dns; unsigned short slot; };
int rfl_net_keep(RflNet *net);
void rfl_net_release(unsigned short *handle);
void rfl_net_stop(unsigned short *handle);
void rfl_http_service(RflNet *net);
void rfl_http_destroy(RflHttpJob *job);
void rfl_server_service(RflNet *net);
void rfl_server_clear(RflNet *net, unsigned short slot);
#endif
