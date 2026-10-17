#ifndef RCC_NATIVE_HOSTED_H
#define RCC_NATIVE_HOSTED_H
/* Native contexts selected explicitly by a hosted kernel. Slot zero is the
 * boot context. No host fork, host stack copying, or runtime scheduler is used.
 * Do not mix these operations with rcc_process_fork/yield/wait/exit. */
int rcc_hosted_clone(unsigned slot); /* 1 in parent, 0 in saved child, -1 on failure */
void rcc_hosted_switch(unsigned slot); /* resumes here when this context is selected again */
void rcc_hosted_drop_switch(unsigned slot); /* discard current continuation; does not return */
void rcc_hosted_exec(void (*entry)(unsigned long), unsigned long argument);
/* Executable packages are octets stored in compatibility cells. A successful
 * load is staged until exec consumes the returned function token. */
unsigned long rcc_hosted_image_load(const void *data, unsigned long cells);
void rcc_hosted_image_discard(unsigned long entry);
/* Stream larger packages without reserving their entire payload in guest RAM.
 * Construction belongs to the current process and does not change its image. */
int rcc_hosted_image_begin(unsigned long cells);
int rcc_hosted_image_append(const void *data, unsigned long cells);
unsigned long rcc_hosted_image_finish(void);
void rcc_hosted_image_cancel(void);
void rcc_hosted_bind_syscall(long (*entry)(unsigned long));
/* Called at user-code checkpoints, never while a kernel callback is active.
 * The kernel owns signal delivery and selection of the next runnable task. */
void rcc_hosted_bind_checkpoint(void (*entry)(void));
int rcc_hosted_stopping(void); /* host SIGTERM requested orderly shutdown */
int rcc_hosted_lock_image(const char *path, unsigned writable);
int rcc_hosted_sync_image(void);
long rcc_hosted_syscall(void *request);
/* Validate an active application's function token before signal dispatch. */
int rcc_hosted_valid_function(unsigned long token, unsigned arity);
/* Console polling returns an octet, -1 for no data, or -2 for EOF. */
int rcc_hosted_input(void);
void rcc_hosted_idle(unsigned milliseconds);
/* Synchronous hosted GX adapter. request contains twelve 32-bit cells pairs;
 * the adapter copies buffers and never retains guest pointers. Kernel only. */
long rcc_hosted_gx(unsigned operation, unsigned long *request);
#endif
