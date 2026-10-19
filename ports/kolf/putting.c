/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2002-2005 Jason Katz-Brown <jasonkb@mit.edu>
 * Copyright 2010 Stefan Majewsky <majewsky@gmx.net>
 * Modified for the C16 port, 2026.
 * Kolf's normal backswing and optional three-click putting, adapted from
 * KolfGame::puttPress/puttRelease/putterTimeout (KDE Kolf, GPL-2.0-or-later).
 * No GUI or clock reads: input timestamps and animation share one timeline. */
#include "putting.h"
#include <string.h>
#include <math.h>
void putt_init(GolfPutt *p,int advanced){memset(p,0,sizeof(*p));p->advanced=advanced;p->pull=9;p->random=1;}
void putt_cancel(GolfPutt *p){p->phase=PUTT_IDLE;p->pull=9;p->strength=p->gauge=0;p->reverse=p->correction=0;}
static unsigned long random_value(GolfPutt *p){p->random=p->random*1664525UL+1013904223UL;return (p->random>>16)&32767UL;}
static void precision(GolfPutt *p){
    if(p->gauge>45||p->gauge< -45){
        int n=(int)p->strength;p->correction=(int)(random_value(p)%90)-45;
        if(n>0)p->strength-=random_value(p)%(unsigned)n;
    }else p->correction=(int)(p->gauge/3);
    p->phase=PUTT_RETURN;
}
void putt_update(GolfPutt *p,unsigned long now){
    /* A complete swing has fewer than 4096 events. Bound pathological clock
     * jumps without letting a stalled UI turn a held key into repeated shots. */
    int events=0;
    while(p->phase!=PUTT_IDLE&&p->phase!=PUTT_FIRE&&(long)(now-p->next)>=0&&events++<4096){
        unsigned interval=20;
        if(p->phase==PUTT_CHARGE){
            if(!p->advanced){p->pull++;p->strength+=1.5;if(p->strength>55)p->phase=PUTT_RETURN;}
            else{
                double amount=pow(2,p->strength/65)-.3;
                if(p->reverse){
                    /* Upstream subtracts pow(...)-1.8 here, which stalls near
                     * 55.1 instead of ever cancelling. Keep the intended sweep
                     * back to zero using the positive charging increment. */
                    p->strength-=amount;
                    if(p->strength<=0){putt_cancel(p);break;}
                }else{
                    p->strength+=amount;if(p->strength>65)p->reverse=1;
                }
                p->pull=9+(int)(p->strength/2);p->gauge=pow(p->strength/65,.8)*360;
                if(p->gauge>360)p->gauge=360;
            }
        }else if(p->phase==PUTT_PRECISION){
            double value=p->gauge;
            value-=.2+p->strength/50+(value>=45?value/100:0);p->gauge=value;
            interval=2;if(value< -45)precision(p);
        }else if(p->phase==PUTT_RETURN){
            if(p->pull<10){p->phase=PUTT_FIRE;break;}
            p->pull--;interval=2;
        }
        p->next+=interval;
    }
    if(events>4096)putt_cancel(p);
}
void putt_press(GolfPutt *p,unsigned long now){
    putt_update(p,now);
    if(p->phase==PUTT_IDLE){putt_cancel(p);p->phase=PUTT_CHARGE;p->next=now+20;}
    else if(p->advanced&&p->phase==PUTT_CHARGE){p->phase=PUTT_PRECISION;p->next=now+20;}
    else if(p->advanced&&p->phase==PUTT_PRECISION){precision(p);p->next=now+2;}
}
void putt_release(GolfPutt *p,unsigned long now){
    putt_update(p,now);
    if(!p->advanced&&p->phase==PUTT_CHARGE)p->phase=PUTT_RETURN;
}
double putt_power(const GolfPutt *p){return p->strength>0?p->strength/8:1;}
