/* Generic guest HTTP CLI and persistent pipe protocol, version 1.
 * Octets occupy native char cells on C16, just like socket/file buffers.
 * Request: u16 big-endian length then URL LF, up to four header LF, blank LF.
 * Response: kind octet, u16 big-endian length, payload. H = final HTTP headers
 * including CRLF separator; B = decoded body; D (zero length) = success;
 * E = diagnostic/failure. Exactly one D or E terminates each request. A caller
 * cancelling mid-response must close/kill the worker rather than resync it.
 * Neither native layouts nor TLS objects cross this interface. */
#ifndef RIPPLE_HTTP_H
#define RIPPLE_HTTP_H
#define RIPPLE_HTTP_REQUEST_MAX 5381
#define RIPPLE_HTTP_HEADERS 'H'
#define RIPPLE_HTTP_BODY 'B'
#define RIPPLE_HTTP_DONE 'D'
#define RIPPLE_HTTP_ERROR 'E'
int ripple_http_main(int argc,char **argv);
#endif
