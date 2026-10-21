/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Single-threaded software profile; unsupported services fail explicitly. */
#include "SDL.h"
#include "SDL_timer_c.h"
#include "platform.h"
#include "audio.h"
#include <string.h>

int SDL_timer_started, SDL_timer_running;
Uint32 SDL_alarm_interval;
SDL_TimerCallback SDL_alarm_callback;
static Uint32 epoch;
int SDL_TimerInit(void) { epoch = SDL_GX_GetTicks(); SDL_timer_started = 1; return 0; }
void SDL_TimerQuit(void) { SDL_timer_started = SDL_timer_running = 0; }
void SDL_StartTicks(void) { epoch = SDL_GX_GetTicks(); }
Uint32 SDL_GetTicks(void) { return SDL_GX_GetTicks() - epoch; }
void SDL_Delay(Uint32 ms) {
    Uint32 remaining = ms, before, elapsed, part;
    SDL_C16AudioPump();
    while (remaining) {
        part = SDL_C16AudioWait(remaining);
        before = SDL_GX_GetTicks();
        SDL_GX_Delay(part);
        elapsed = SDL_GX_GetTicks() - before;
        SDL_C16AudioPump();
        if (!elapsed || elapsed >= remaining) break;
        remaining -= elapsed;
    }
}
int SDL_SetTimer(Uint32 ms, SDL_TimerCallback callback) {
    (void)callback;
    if (!ms) return 0;
    SDL_SetError("SDL timer callbacks are unavailable in the single-threaded profile"); return -1;
}
SDL_TimerID SDL_AddTimer(Uint32 ms, SDL_NewTimerCallback callback, void *data) {
    (void)ms; (void)callback; (void)data;
    SDL_SetError("SDL timer callbacks are unavailable in the single-threaded profile"); return NULL;
}
SDL_bool SDL_RemoveTimer(SDL_TimerID id) { (void)id; return SDL_FALSE; }
int SDL_SetTimerThreaded(int value) {
    if (value) { SDL_SetError("SDL event threads are unavailable"); return -1; } return 0;
}
void SDL_ThreadedTimerCheck(void) {}
void SDL_InstallParachute(void) {}
void SDL_UninstallParachute(void) {}
void SDL_Audio_SetCaption(const char *title) { (void)title; }
SDL_bool SDL_HasMMX(void) { return SDL_FALSE; }

SDL_Thread *SDL_CreateThread(int (*fn)(void *), void *data) {
    (void)fn; (void)data; SDL_SetError("SDL threads are unavailable"); return NULL;
}
Uint32 SDL_ThreadID(void) { return 1; }
Uint32 SDL_GetThreadID(SDL_Thread *thread) { (void)thread; return 1; }
void SDL_WaitThread(SDL_Thread *thread,int *status) { (void)thread; if(status)*status=-1; }
void SDL_KillThread(SDL_Thread *thread) { (void)thread; }
/* The upstream event core uses these only in its disabled threaded paths. */
SDL_mutex *SDL_CreateMutex(void) { SDL_SetError("SDL mutexes are unavailable"); return NULL; }
int SDL_mutexP(SDL_mutex *m) { (void)m; return 0; }
int SDL_mutexV(SDL_mutex *m) { (void)m; return 0; }
void SDL_DestroyMutex(SDL_mutex *m) { (void)m; }

size_t SDL_strlcpy(char *dst,const char *src,size_t size) {
    size_t n=strlen(src), copy=n;
    if (size) { if(copy>=size)copy=size-1; memcpy(dst,src,copy);dst[copy]=0; } return n;
}
size_t SDL_strlcat(char *dst,const char *src,size_t size) {
    size_t n=0;while(n<size && dst[n])n++;
    return n+SDL_strlcpy(dst+n,src,size-n);
}
void *SDL_revcpy(void *dst,const void *src,size_t count) { return memmove(dst,src,count); }

#if SDL_RIPLIX_C16
#include "SDL_video.h"
int SDL_RLESurface(SDL_Surface *s) { (void)s; return -1; }
void SDL_UnRLESurface(SDL_Surface *s,int recode) { (void)recode;s->flags&=~SDL_RLEACCEL; }
int SDL_RLEBlit(SDL_Surface *s,SDL_Rect *sr,SDL_Surface *d,SDL_Rect *dr) {
    (void)s;(void)sr;(void)d;(void)dr; SDL_SetError("RLE is unavailable");return -1;
}
int SDL_RLEAlphaBlit(SDL_Surface *s,SDL_Rect *sr,SDL_Surface *d,SDL_Rect *dr) {
    return SDL_RLEBlit(s,sr,d,dr);
}
#endif
int SDL_SoftStretch(SDL_Surface *s,SDL_Rect *sr,SDL_Surface *d,SDL_Rect *dr) {
    (void)s;(void)sr;(void)d;(void)dr;SDL_SetError("Stretch is outside the initial SDL profile");return -1;
}
int SDL_SetGamma(float r,float g,float b) {
    (void)r;(void)g;(void)b;SDL_SetError("Gamma control is unavailable");return -1;
}
int SDL_SetGammaRamp(const Uint16 *r,const Uint16 *g,const Uint16 *b) {
    (void)r;(void)g;(void)b;SDL_SetError("Gamma control is unavailable");return -1;
}
int SDL_GetGammaRamp(Uint16 *r,Uint16 *g,Uint16 *b) {
    (void)r;(void)g;(void)b;SDL_SetError("Gamma control is unavailable");return -1;
}
