/* SPDX-License-Identifier: MIT */
#ifndef FUSE_CHAT_HTTP_H
#define FUSE_CHAT_HTTP_H
#include <stdio.h>
typedef int (*ChatBody)(void *, const char *, unsigned);
/* Verified HTTPS only, no redirect or plaintext credential forwarding. */
int chat_http(const char *origin, const char *path, const char *token, const char *content_type,
              FILE *body, const char *ca, ChatBody receive, void *context, char *error,
              unsigned error_size);
#endif
