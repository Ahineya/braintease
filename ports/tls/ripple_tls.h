#ifndef RIPPLE_TLS_H
#define RIPPLE_TLS_H
#include <stddef.h>
#include <sys/types.h>
/* Guest-side verified TLS over an already connected TCP descriptor.
 * The caller owns fd; close releases TLS state and restores its file flags.
 * Parsed anchors are reused within a process after checking file contents.
 * A child rebuilds its context and cannot use an inherited connection. Each
 * connection still creates a new TLS session and seeds its RNG. This synchronous facade can be reused by other small clients;
 * event-loop clients can use wolfSSL's I/O callback and WANT_* APIs directly. */
typedef struct ripple_tls RippleTLS;
#define RIPPLE_TLS_CA_FILE "/etc/ssl/cert.pem"
RippleTLS *ripple_tls_open(int fd, const char *hostname, const char *ca_file,
                          unsigned long timeout_ms, volatile int *cancelled);
/* Browser clients can poll their UI while waiting for socket readiness. */
RippleTLS *ripple_tls_open_poll(int fd, const char *hostname, const char *ca_file,
                               unsigned long timeout_ms, int (*poll)(void *), void *context);
/* Prepare trust and TLS configuration before opening TCP. Slow guests must
 * not spend the server's handshake deadline parsing their local CA bundle.
 * fd is an unconnected socket; dial is called once after preparation and
 * returns zero on success. Its context is borrowed only during this call. */
RippleTLS *ripple_tls_open_dial(int fd, const char *hostname, const char *ca_file,
                               unsigned long timeout_ms, int (*dial)(void *), void *context);
ssize_t ripple_tls_read(RippleTLS *, void *, size_t, unsigned long timeout_ms);
int ripple_tls_write(RippleTLS *, const void *, size_t, unsigned long timeout_ms);
unsigned ripple_tls_pending(RippleTLS *);
const char *ripple_tls_error(void);
void ripple_tls_close(RippleTLS *);
#endif
