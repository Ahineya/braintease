/* SPDX-License-Identifier: MIT */
#ifndef JPIPE_H
#define JPIPE_H
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <stdint.h>
#include <time.h>
#include <math.h>
#include "quickjs.h"
#ifdef QJS_C16_H
#include "qjs-platform.h"
#endif
enum { J_JSONL, J_TEXT, J_DOCUMENT };
enum { J_MAP, J_FILTER, J_REDUCE, J_SORT };
typedef struct {
    int kind;
    const char *source, *seed;
    JSValue function, accumulator, *values;
    size_t used, capacity;
    unsigned long records;
} JStage;
typedef struct {
    JSRuntime *rt;
    JSContext *ctx;
    JStage *stages;
    size_t count, capacity, active;
    int input, output, failed, timed_out, async_used;
    size_t memory, record_limit;
    unsigned long polls, poll_limit, time_limit, input_record;
    uint64_t deadline;
    JSValue replacer;
} JPipe;
typedef struct {
    JPipe *pipe;
    char *data;
    size_t length, capacity;
    int look;
} JReader;
int j_error(JPipe *, const char *);
int j_exception(JPipe *);
void j_budget(JPipe *);
int j_runtime(JPipe *);
void j_dispose(JPipe *);
int j_push(JPipe *, size_t, JSValue); /* Consumes the value, including on error. */
int j_finish(JPipe *);
int j_read(JPipe *);
#endif
