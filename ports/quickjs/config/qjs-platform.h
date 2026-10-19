/* SPDX-License-Identifier: MIT */
#ifndef QJS_C16_PLATFORM_H
#define QJS_C16_PLATFORM_H
#include "quickjs.h"

/* Terminal applications own a 64,000-cell stack bank on RVM/Riplix. Use
 * 48,000 cells for native distance + charged JS frames, retaining 16,000
 * cells for unchecked C callees, exception reporting and signal delivery.
 * The general embedding header keeps its smaller default; this is an
 * explicit terminal-host policy, also safe within Fuse-X's larger stack.
 */
#define QJS_C16_TERMINAL_STACK_CELLS 48000UL

/* Experimental platform hooks, not an installed embedding SDK. All allocation
 * and stack sizes are C16 cells (two physical octets). The allocator's limit
 * includes its own header and all alignment slack, but not the system
 * allocator's private bookkeeping. Returned storage is four-cell aligned;
 * core atom and autoinit pointer tags depend on that guarantee.
 * One logical ArrayBuffer octet occupies one cell; core_typed_arrays.py
 * supplies explicit scalar accessors instead of native pointer casts.
 */
typedef struct QJSAllocatorControl {
    size_t calls;
    size_t fail_at; /* zero disables fault injection */
    size_t peak_cells;
} QJSAllocatorControl;

extern const JSMallocFunctions qjs_c16_malloc_functions;
size_t qjs_c16_allocation_overhead(void);
uint64_t qjs_c16_physical_octets(size_t cells);

/* Banked pointer integers cannot be subtracted as linear addresses. Stack
 * growth is upward in both guests; distance also permits a future downward
 * implementation. Capture the base inside the caller's active stack frame.
 * A zero limit explicitly disables the guard, following QuickJS's API.
 */
uintptr_t qjs_c16_linear_address(const void *pointer);
long qjs_c16_stack_at(uintptr_t base, size_t limit, size_t requested,
                      uintptr_t here);
long qjs_c16_check_stack(uintptr_t base, size_t limit, size_t requested);
#endif
