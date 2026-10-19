/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef DOOM_GUEST_AUDIO_H
#define DOOM_GUEST_AUDIO_H
#include <sys/pcm.h>
int Doom_GuestAudioOpen(unsigned long rate, unsigned frames);
void Doom_GuestAudioStart(void);
void Doom_GuestAudioSubmit(const short *samples, unsigned frames);
void Doom_GuestAudioClose(void);
int Doom_GuestAudioFd(void);
unsigned long Doom_GuestAudioDropped(void);
void Doom_GuestAudioPump(void);
int Doom_GuestAudioMix(struct pcm_mix_voice *voices, unsigned count, short *samples, unsigned frames);
int Doom_GuestAudioNeeded(unsigned frames);
#endif
