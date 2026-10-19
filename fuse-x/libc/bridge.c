/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <native_hosted.h>
#include "user.h"
long ripple_syscall(struct request *request) { return rcc_hosted_syscall(request); }
/* The hosted kernel dispatches native handlers after completing a syscall.
 * Keep the registration token required by the shared Riplix libc ABI. */
void ripple_signal_entry(void) {}
/* Native pointers are already absolute logical bank/offset values. */
void *ripple_absolute_pointer(void *pointer) { return pointer; }
