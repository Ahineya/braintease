/* Serialized, in-process embedding interface. No term IDs escape this API. */
#ifndef SYMA_SESSION_H
#define SYMA_SESSION_H
#include <stdint.h>
typedef struct SymaContext SymaContext;
typedef struct {
    char *text; /* owned UTF-8 normal form, or declaration acknowledgement */
    char error[256];
    uint32_t rewrites, primitives;
} SymaResult;
/* One runtime per process. Contexts have independent rules/bindings and share
 * immutable storage. Calls must be serialized; do not mix with the CLI REPL. */
void syma_runtime_open(const char *executable);
void syma_runtime_close(void);
SymaContext *syma_context_new(uint32_t maxsteps);
void syma_context_free(SymaContext *context);
/* Change the per-expression rewrite budget without losing definitions. Zero
 * selects the same default as syma_context_new(). */
void syma_context_set_limit(SymaContext *context, uint32_t maxsteps);
/* One complete term per cell. Import, Module, R and Define are declarations.
 * All other terms normalize without dispatching host effects. Failure restores
 * the previous context. Results remain valid until syma_result_free(). */
int syma_context_eval(SymaContext *context, const char *source, SymaResult *result);
void syma_result_free(SymaResult *result);
#endif
