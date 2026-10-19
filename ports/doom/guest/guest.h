/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef DOOM_GUEST_H
#define DOOM_GUEST_H
#include <stdint.h>
extern char *doom_guest_wad_path;
extern unsigned long doom_guest_frames;
extern int doom_guest_fast;
extern int doom_guest_stats;
void Doom_GuestGraphics(void);
void Doom_GuestShutdown(void);
void Doom_GuestInput(void);
void Doom_GuestTick(uint32_t tick);
void Doom_GuestPresent(const uint16_t *pixels, uint32_t frame);
int Doom_GuestIndexed(void);
void Doom_GuestPresentIndexed(const unsigned char *pixels, const uint16_t *palette, uint32_t frame);
#endif
