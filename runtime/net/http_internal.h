#ifndef RIPPLE_HTTP_INTERNAL_H
#define RIPPLE_HTTP_INTERNAL_H
#include <net_http.h>
#include <string.h>
int http_line_octet(NetHttpParser *head, unsigned short octet);
int http_field(NetHttpParser *head, char **value);
int http_token(unsigned short c);
int http_equal(const char *a, const char *b);
int http_decimal(const char *text, unsigned long *value);
unsigned short http_format_decimal(char *output, unsigned long value);
int http_request(NetHttp *http, const char *host, const char *path, unsigned short port);
int http_address(const char *host, unsigned char *address);
int http_fail(NetHttp *http, int error);
#endif
