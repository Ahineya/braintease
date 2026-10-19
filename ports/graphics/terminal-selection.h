/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef RIPPLE_TERMINAL_SELECTION_H
#define RIPPLE_TERMINAL_SELECTION_H
#include <stdlib.h>
#include "terminal.h"
struct vt_selection { unsigned anchor, end, active, dragging; };
static unsigned vt_selection_at(struct vt *t,int x,int y) {
    if(x<0)x=0;if(y<0)y=0;
    if(x>=(int)t->cols)x=t->cols-1;if(y>=(int)t->rows)y=t->rows-1;
    return (unsigned)y*t->cols+x;
}
static int vt_selected(struct vt_selection *s,unsigned at) {
    return s->active&&at>=(s->anchor<s->end?s->anchor:s->end)&&at<=(s->anchor>s->end?s->anchor:s->end);
}
static unsigned vt_selection_colour(struct vt_selection *s,unsigned at,unsigned colour) {
    return vt_selected(s,at)?(colour&~255U)|((colour&15)<<4)|((colour>>4)&15):colour;
}
static char *vt_selection_text(struct vt *t,struct vt_selection *s,size_t *length) {
    struct vt_cell *cells=vt_cells(t);char *text;
    unsigned first,last,at,end,i,n,k;unsigned long used=0;unsigned char encoded[4];
    *length=0;if(!s->active)return NULL;
    first=s->anchor<s->end?s->anchor:s->end;last=s->anchor>s->end?s->anchor:s->end;
    if(last>=t->cols*t->rows)return NULL;
    if(cells[first].ch==RU_CONTINUATION&&first%t->cols)first--;
    text=malloc(32769UL);if(!text)return NULL;
    for(at=first;at<=last;) {
        end=(at/t->cols+1)*t->cols;if(end>last+1)end=last+1;
        i=end;while(i>at&&cells[i-1].ch==' ')i--;
        for(;at<i;at++)if(cells[at].ch!=RU_CONTINUATION) {
            unsigned count=ru_scalar_count(cells[at].ch),j;
            for(j=0;j<count;j++) {
                n=ru_encode(ru_scalar(cells[at].ch,j),encoded);
                if(n>32768UL-used){free(text);return NULL;}
                for(k=0;k<n;k++)text[used++]=encoded[k];
            }
        }
        at=end;
        if(at<=last){if(used==32768UL){free(text);return NULL;}text[used++]='\n';}
    }
    text[used]=0;*length=used;return text;
}
#endif
