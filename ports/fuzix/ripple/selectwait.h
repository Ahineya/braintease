/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef RIPPLE_SELECTWAIT_H
#define RIPPLE_SELECTWAIT_H
#include <vm_devices.h>
/* Called under IRQ protection by both platform clocks, using the host sample
 * they already read. NULL wakes timed selectors with a clock error. */
void ripple_select_clock(const VmHostTime *);
#endif
