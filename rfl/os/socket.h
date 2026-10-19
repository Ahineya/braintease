/* Shared private C16 engine records. Managed RFL values never enter these structs. */
#ifndef RFL_GUEST_SOCKET_ENGINE_H
#define RFL_GUEST_SOCKET_ENGINE_H
#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#define RFL_IO_SLOTS 32
#define RFL_IO_WAIT 0
#define RFL_IO_DNS 1
#define RFL_IO_CONNECT 2
#define RFL_IO_READ 3
#define RFL_IO_WRITE 4
#define RFL_IO_READY 1
#define RFL_IO_CANCELLED -1
#define RFL_IO_TIMED_OUT -2
#define RFL_IO_OS_ERROR -3
#define RFL_IO_DNS_ERROR -4
typedef struct RflSocket { unsigned long refs; int fd; unsigned reading, writing; } RflSocket;
typedef struct RflWait RflWait;
typedef struct RflDns RflDns;
typedef struct RflPoll { unsigned long refs; unsigned driver; RflWait *slots[RFL_IO_SLOTS]; } RflPoll;
struct RflWait {
    RflPoll *poll;
    RflSocket *socket, *result_socket;
    RflDns *dns;
    unsigned slot, events, kind, active, started, notified, has_timeout;
    int status, error;
    unsigned long duration, deadline, size, offset;
    unsigned char *buffer;
    struct sockaddr_in peer;
};
void *rfl_socket_tcp(void);
int rfl_socket_errno(void);
void *rfl_socket_retain(void *);
void rfl_socket_release(void *);
int rfl_socket_descriptor(void *);
void *rfl_socket_from_fd(int);
void *rfl_socket_pair(void);
void *rfl_socket_pair_take(void *, unsigned);
void rfl_socket_pair_drop(void *);
int rfl_socket_connect(void *, const unsigned char *, unsigned);
int rfl_socket_finish(void *);
long rfl_socket_read(void *, unsigned char *, unsigned long);
long rfl_socket_write(void *, const unsigned char *, unsigned long);
int rfl_socket_shutdown(void *, unsigned);
int rfl_socket_resolve(const unsigned char *, unsigned char *);
void *rfl_poll_create(void);
void *rfl_poll_retain(void *);
void rfl_poll_release(void *);
void *rfl_poll_driver(void *);
void rfl_poll_driver_drop(void *);
void *rfl_poll_watch(void *, void *, unsigned);
unsigned rfl_poll_slot(void *);
int rfl_poll_error(void *);
int rfl_poll_timeout(void *, unsigned long);
int rfl_poll_status(void *, unsigned long);
void rfl_poll_cancel(void *);
void rfl_poll_drop(void *);
int rfl_poll_clock(unsigned long *);
/* Final output needs 33 cells: count followed by at most 32 ready slot indices. */
int rfl_poll_turn(void *, unsigned long, unsigned long *, unsigned short *);
void *rfl_poll_connect(void *, const unsigned char *, unsigned);
void *rfl_poll_connect_result(void *);
void *rfl_poll_transfer(void *, void *, unsigned, const unsigned char *, unsigned long);
unsigned long rfl_poll_size(void *);
int rfl_poll_copy(void *, unsigned char *, unsigned long);
void *rfl_poll_resolve(void *, const unsigned char *);
int rfl_poll_resolve_result(void *, unsigned char *);
RflWait *rfl_io_create(RflPoll *, RflSocket *, unsigned, unsigned);
void rfl_io_finish(RflWait *, int, int);
unsigned long rfl_io_remaining(unsigned long, unsigned long);
int rfl_dns_fd(RflWait *);
unsigned rfl_dns_events(RflWait *);
unsigned long rfl_dns_delay(RflWait *, unsigned long, unsigned long);
void rfl_dns_step(RflWait *, unsigned long);
void rfl_dns_close(RflWait *);
void rfl_dns_destroy(RflWait *);
#endif
