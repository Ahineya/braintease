#ifndef RIPPLE_HTTP_INTERNAL_H
#define RIPPLE_HTTP_INTERNAL_H
#include <net_http.h>
#include <string.h>
int http_request(NetHttp *http, const char *host, const char *path, unsigned short port);
int http_address(const char *host, unsigned char *address);
int http_fail(NetHttp *http, int error);
#endif
