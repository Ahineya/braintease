/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Private versioned wire protocol. Every carrier is one logical octet, never
 * a native C structure or pointer. Both endpoints compile against this file. */
#ifndef SDL_MIXER_IPC_H
#define SDL_MIXER_IPC_H
#include "SDL.h"
#define MX_HEADER 40
#define MX_PAYLOAD 1024
#define MX_REPLY 0x80000000UL
#define MX_TIMEOUT 10000UL
#define MX_DEFAULT_HELPER "/usr/bin/sdl-mixerd"
enum {
    MX_OPEN=1, MX_ALLOCATE, MX_RESERVE, MX_BEGIN, MX_DATA, MX_COMMIT,
    MX_DROP, MX_PLAY, MX_HALT, MX_EXPIRE, MX_VOLUME, MX_CHUNK_VOLUME,
    MX_PAUSE, MX_RESUME, MX_PAUSED, MX_PLAYING, MX_FADEOUT, MX_FADING,
    MX_GETCHUNK, MX_POLL, MX_QUIT
};
typedef struct MX_Message {
    Uint32 request, operation, length;
    Sint32 arg[6];
    Uint8 payload[MX_PAYLOAD];
} MX_Message;
static Uint32 mx_get32(const Uint8 *p) {
    return (Uint32)p[0] | ((Uint32)p[1]<<8) | ((Uint32)p[2]<<16) | ((Uint32)p[3]<<24);
}
static void mx_put32(Uint8 *p, Uint32 n) {
    p[0]=(Uint8)(n&255);p[1]=(Uint8)((n>>8)&255);
    p[2]=(Uint8)((n>>16)&255);p[3]=(Uint8)((n>>24)&255);
}
static void mx_encode(Uint8 *p, const MX_Message *m) {
    int i;
    p[0]='M';p[1]='X';p[2]='P';p[3]='1';
    mx_put32(p+4,m->request);mx_put32(p+8,m->operation);mx_put32(p+12,m->length);
    for(i=0;i<6;i++) mx_put32(p+16+i*4,(Uint32)m->arg[i]);
}
static int mx_decode(MX_Message *m,const Uint8 *p) {
    int i;
    if(p[0]!='M'||p[1]!='X'||p[2]!='P'||p[3]!='1')return -1;
    m->request=mx_get32(p+4);m->operation=mx_get32(p+8);m->length=mx_get32(p+12);
    if(!m->request||m->length>MX_PAYLOAD)return -1;
    for(i=0;i<6;i++)m->arg[i]=(Sint32)mx_get32(p+16+i*4);
    return 0;
}
#endif
