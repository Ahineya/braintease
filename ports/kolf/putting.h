/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef KOLF_PUTTING_H
#define KOLF_PUTTING_H
enum PuttPhase { PUTT_IDLE, PUTT_CHARGE, PUTT_PRECISION, PUTT_RETURN, PUTT_FIRE };
typedef struct GolfPutt {
    int advanced,phase,pull,reverse,correction;
    double strength,gauge;
    unsigned long next,random;
} GolfPutt;
void putt_init(GolfPutt *p,int advanced);
void putt_cancel(GolfPutt *p);
void putt_press(GolfPutt *p,unsigned long now);
void putt_release(GolfPutt *p,unsigned long now);
void putt_update(GolfPutt *p,unsigned long now);
double putt_power(const GolfPutt *p);
#endif
