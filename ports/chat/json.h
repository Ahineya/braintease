/* SPDX-License-Identifier: MIT */
#ifndef FUSE_CHAT_JSON_H
#define FUSE_CHAT_JSON_H
#include <stddef.h>
#include <stdio.h>
#define CJ_TOKENS 4096
typedef struct {
    unsigned long start, end;
    unsigned after;
    char kind;
} CJToken;
typedef struct {
    const char *text;
    unsigned long size, at;
    CJToken *tokens;
    unsigned count;
} CJ;
int cj_parse(CJ *, const char *, unsigned long);
void cj_free(CJ *);
int cj_field(const CJ *, unsigned, const char *);
int cj_equal(const CJ *, unsigned, const char *);
char *cj_string(const CJ *, unsigned);
int cj_number(const CJ *, unsigned, unsigned long *);
int cj_quote(FILE *, const char *);
int cj_utf8(const char *, unsigned long);
void cj_print(FILE *, const char *);
#endif
