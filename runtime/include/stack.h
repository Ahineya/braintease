#ifndef RCC_STACK_H
#define RCC_STACK_H
#include <stddef.h>

/* One context per independently scheduled stack. Keep it alive until destroy.
 * Compile C callers/callees with --segmented-stack and use a matching runtime.
 * Growth itself never calls malloc and never uses a shared emergency stack. */
typedef struct rcc_stack_context {
    unsigned free_bank;
    unsigned active_segments;
    unsigned peak_segments;
    unsigned root_bank;
    void *storage;
} rcc_stack_context;

/* Reserve a pool from malloc before making calls that can overflow the root.
 * Returns 0 on success, -1 on invalid configuration/allocation failure. */
int rcc_stack_reserve(rcc_stack_context *context, unsigned segments);

/* OS alternative: supply consecutive, exclusively owned RAM banks. These must
 * not overlap the root stack, globals, live objects, or an allocator's arena. */
int rcc_stack_init(rcc_stack_context *context, unsigned first_bank, unsigned count);

/* Only on the root stack, after all grown segments have unwound. */
int rcc_stack_destroy(rcc_stack_context *context);
#endif
