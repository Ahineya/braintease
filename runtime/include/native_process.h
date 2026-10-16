#ifndef RCC_NATIVE_PROCESS_H
#define RCC_NATIVE_PROCESS_H
#include <stddef.h>

/* Experimental cooperative processes for --target native. Referencing one of
 * these services opts the whole linked program into resumable native frames.
 * No host fork(), thread stacks, or Ripple instruction interpreter are used. */
int rcc_process_fork(void);                 /* parent: child PID, child: 0 */
int rcc_process_getpid(void);
void rcc_process_yield(void);
/* Wait for a direct child, or any child with pid == -1. Returns its PID, or -1
 * if there is no matching child. status receives the raw 16-bit exit code. */
int rcc_process_wait(int pid, int *status);
void rcc_process_exit(int status);

/* Trusted shared kernel state. Private malloc storage is copied on write after
 * fork; these allocations and their allocator are shared by all processes.
 * Allocation sizes use compatibility cells. Returns NULL on exhaustion.
 * Free with kernel_free, rather than free. This is not a protection boundary. */
void *rcc_process_kernel_alloc(size_t cells);
void rcc_process_kernel_free(void *ptr);

#endif
