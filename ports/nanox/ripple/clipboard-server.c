/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include "serv.h"
#include "nxproto.h"
#include "server.h"
#include "clipboard-wire.h"
#ifdef NXR_HOSTED
#include "native.h"
#endif
struct transfer { char *text; unsigned long length, at; unsigned op; GR_WINDOW_ID window; };
static struct transfer transfers[FD_SETSIZE];
#ifndef NXR_HOSTED
static char *local_text;
static unsigned long local_length;
#endif
void nxr_clipboard_forget(int fd) {
    if(fd>=0&&fd<FD_SETSIZE){free(transfers[fd].text);memset(transfers+fd,0,sizeof(*transfers));}
}
int nxr_clipboard_valid(void *packet,unsigned size) {
    struct nxr_clip_req *q=packet;
    if(size<sizeof(*q)||q->version!=1||q->length>NXR_CLIPBOARD_MAX)return 0;
    if(q->op==NXR_CLIP_WRITE)return q->count&&q->count<=NXR_CLIPBOARD_CHUNK&&size==sizeof(*q)+q->count;
    return q->op>=NXR_CLIP_GET&&q->op<=NXR_CLIP_END&&!q->count&&size==sizeof(*q);
}
/* Strict UTF-8 and one octet per C16 cell; never publish a partial paste or
 * embedded NUL. The same text contract applies to native and VM clipboards. */
static int valid_text(const char *s,unsigned long length) {
    unsigned long i=0,scalar;unsigned c,n,j,min;
    while(i<length) {
        c=(unsigned char)s[i++];if(!c||c>255)return 0;if(c<128)continue;
        n=c>=0xc2&&c<=0xdf?1:c>=0xe0&&c<=0xef?2:c>=0xf0&&c<=0xf4?3:0;
        if(!n||n>length-i)return 0;
        min=n==1?0x80:n==2?0x800:0;scalar=c&((1U<<(6-n))-1);
        for(j=0;j<n;j++){c=(unsigned char)s[i++];if(c>255||(c&0xc0)!=0x80)return 0;scalar=(scalar<<6)|(c&63);}
        if(scalar<(n==3?0x10000UL:min)||scalar>0x10ffffUL||(scalar>=0xd800&&scalar<=0xdfff))return 0;
    }
    return 1;
}
void nxr_clipboard_dispatch(int fd,void *packet) {
    struct nxr_clip_req *q=packet;struct nxr_clip_reply reply;
    struct transfer *t;GR_WINDOW *w;short type=NXR_CLIPBOARD;
    memset(&reply,0,sizeof(reply));reply.status=1;
    if(fd<0||fd>=FD_SETSIZE)return;
    t=transfers+fd;curclient=GsFindClient(fd);
    if(!curclient)goto done;
    /* A destroyed window must not prevent cancelling this connection's own
     * staging allocation. No clipboard content is read or written by END. */
    if(q->op==NXR_CLIP_END){nxr_clipboard_forget(fd);reply.status=0;goto done;}
    w=GsFindWindow(q->window);if(!w||w->owner!=curclient)goto done;
    if(q->op==NXR_CLIP_GET||q->op==NXR_CLIP_SET) {
        nxr_clipboard_forget(fd);
        t->text=malloc(q->op==NXR_CLIP_GET?NXR_CLIPBOARD_MAX+1:q->length+1);
        if(!t->text)goto done;
        t->window=q->window;t->op=q->op;t->length=q->length;
        if(q->op==NXR_CLIP_GET) {
#ifdef NXR_HOSTED
            if(nxh_clipboard(w,t->text,&t->length,0)<0){nxr_clipboard_forget(fd);goto done;}
#else
            t->length=local_length;if(local_length)memcpy(t->text,local_text,local_length);
#endif
            reply.length=t->length;
        }
        reply.status=0;
    } else if(t->text&&t->window==q->window) {
        if(q->op==NXR_CLIP_READ&&t->op==NXR_CLIP_GET) {
            reply.count=t->length-t->at>NXR_CLIPBOARD_CHUNK?NXR_CLIPBOARD_CHUNK:t->length-t->at;
            memcpy(reply.text,t->text+t->at,reply.count);t->at+=reply.count;reply.status=0;
        } else if(q->op==NXR_CLIP_WRITE&&t->op==NXR_CLIP_SET&&q->count<=t->length-t->at) {
            memcpy(t->text+t->at,q+1,q->count);t->at+=q->count;reply.status=0;
        } else if(q->op==NXR_CLIP_COMMIT&&t->op==NXR_CLIP_SET&&t->at==t->length&&valid_text(t->text,t->length)) {
#ifdef NXR_HOSTED
            if(nxh_clipboard(w,t->text,&t->length,1)==0)reply.status=0;
#else
            free(local_text);local_text=t->text;local_length=t->length;t->text=NULL;reply.status=0;
#endif
            nxr_clipboard_forget(fd);
        }
    }
done:
    nxr_write(fd,&type,sizeof(type));nxr_write(fd,&reply,sizeof(reply));
}
