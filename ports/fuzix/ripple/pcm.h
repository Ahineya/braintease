/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef RIPPLE_PCM_H
#define RIPPLE_PCM_H
#include <vm_devices.h>
int ripple_pcm_open(uint_fast8_t, uint16_t);
int ripple_pcm_close(uint_fast8_t);
int ripple_pcm_write(uint_fast8_t, uint_fast8_t, uint_fast8_t);
int ripple_pcm_ioctl(uint_fast8_t, uarg_t, char *);
unsigned ripple_pcm_status(void);
void ripple_pcm_clock(const VmHostTime *);
void ripple_pcm_exit(ptptr);
#endif
