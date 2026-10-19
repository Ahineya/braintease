/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef RIPPLE_TERMINAL_INPUT_H
#define RIPPLE_TERMINAL_INPUT_H
#include <stdlib.h>
#include <string.h>
/* One ordered queue for keys, terminal replies and whole pastes. PTY writes
 * consume only bytes actually accepted; paste size is independent of its ring. */
#define VT_INPUT_MAX 65536UL
struct vt_input { char *text; unsigned long head, length, capacity; };
static int vt_input_reserve(struct vt_input *q,unsigned long extra) {
    char *next;unsigned long capacity;
    if(extra>VT_INPUT_MAX-q->length)return -1;
    if(q->head){memmove(q->text,q->text+q->head,q->length);q->head=0;}
    if(extra<=q->capacity-q->length)return 0;
    capacity=q->capacity?q->capacity:512;
    while(capacity<q->length+extra)capacity*=2;
    next=realloc(q->text,capacity);if(!next)return -1;
    q->text=next;q->capacity=capacity;return 0;
}
static int vt_input_put(struct vt_input *q,const char *text,unsigned long length) {
    if(vt_input_reserve(q,length))return -1;
    if(length)memcpy(q->text+q->length,text,length);q->length+=length;return 0;
}
static void vt_input_consume(struct vt_input *q,unsigned long length) {
    q->head+=length;q->length-=length;if(!q->length)q->head=0;
}
static int vt_input_paste(struct vt_input *q,const char *text,unsigned long length,int bracketed) {
    unsigned long i;unsigned c;
    /* Reserve the entire operation before modifying the queue. Untrusted
     * clipboard controls never become terminal escape sequences or signals. */
    if(length>32768UL||vt_input_reserve(q,length+(bracketed?12:0)))return -1;
    if(bracketed){memcpy(q->text+q->length,"\033[200~",6);q->length+=6;}
    for(i=0;i<length;i++) {
        c=(unsigned char)text[i];
        if(c=='\r'){if(i+1<length&&text[i+1]=='\n')i++;c=bracketed?'\n':'\r';}
        else if(c=='\n')c=bracketed?'\n':'\r';
        else if(c<32&&c!='\t')continue;
        else if(c==127)continue;
        q->text[q->length++]=c;
    }
    if(bracketed){memcpy(q->text+q->length,"\033[201~",6);q->length+=6;}
    return 0;
}
#endif
