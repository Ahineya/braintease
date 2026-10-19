/* SPDX-License-Identifier: MIT */
#ifndef FUSE_QJS_SYSTEM_H
#define FUSE_QJS_SYSTEM_H
#include "quickjs.h"

typedef struct QJSSystem QJSSystem;
/* Optional POSIX terminal capabilities, deliberately separate from the engine.
 * One instance owns the context's opaque pointer. All JS handles and queued
 * buffers count against its runtime allocator. Dispose before the context.
 * Installs fuse:system and global setTimeout/clearTimeout. */
QJSSystem *qjs_system_new(JSContext *ctx);
/* Resolve optional source modules from an embedding host's module loader. */
JSModuleDef *qjs_system_module(JSContext *ctx, const char *name);
void qjs_system_free(QJSSystem *system);
void qjs_system_cancel(QJSSystem *system);
int qjs_system_pending(QJSSystem *system);
/* Advance one bounded unit of OS work; wait at most max_wait_ms in select.
 * Return 0/1 for idle/progress, -1 with a pending JS exception on failure.
 * The embedding host must alternate this with bounded Promise-job batches. */
int qjs_system_step(QJSSystem *system, unsigned long max_wait_ms);
int qjs_system_now(uint64_t *milliseconds);
#endif
