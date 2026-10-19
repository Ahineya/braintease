/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef RIPPLE_TERMINAL_SELECTION_H
#define RIPPLE_TERMINAL_SELECTION_H
#include <stdlib.h>
#include <string.h>
#include "terminal.h"
struct vt_selection {
    unsigned anchor,end,active,dragging,cols,rows,alternate;
    struct vt_cell *snapshot;
};
static void vt_selection_clear(struct vt_selection *s) {
    free(s->snapshot);memset(s,0,sizeof(*s));
}
/* Save visible text independently of incoming ANSI. Cursor/colour changes
 * and output elsewhere must not cancel a selection or change copied text. */
static int vt_selection_begin(struct vt *t,struct vt_selection *s,unsigned at) {
    struct vt_cell *snapshot=malloc((unsigned long)t->cols*t->rows*sizeof(*snapshot));
    vt_selection_clear(s);if(!snapshot)return -1;
    memcpy(snapshot,vt_cells(t),(unsigned long)t->cols*t->rows*sizeof(*snapshot));
    s->snapshot=snapshot;s->cols=t->cols;s->rows=t->rows;s->alternate=t->alternate;
    s->anchor=s->end=at;s->active=s->dragging=1;return 0;
}
/* Validate only completed synchronized frames, so an erase followed by a
 * repaint in another PTY packet cannot invalidate otherwise identical text.
 * During a drag the range follows the live screen; once released, changes
 * to its selected characters invalidate it. Return whether it was cleared. */
static int vt_selection_update(struct vt *t,struct vt_selection *s) {
    struct vt_cell *cells;unsigned first,last,i;
    if(!s->active||!s->snapshot)return 0;
    if(t->cols!=s->cols||t->rows!=s->rows||t->alternate!=s->alternate){vt_selection_clear(s);return 1;}
    if(t->synchronized)return 0;
    cells=vt_cells(t);
    if(s->dragging){memcpy(s->snapshot,cells,(unsigned long)t->cols*t->rows*sizeof(*cells));return 0;}
    first=s->anchor<s->end?s->anchor:s->end;last=s->anchor>s->end?s->anchor:s->end;
    if(last>=t->cols*t->rows){vt_selection_clear(s);return 1;}
    /* A selection of a wide continuation also copies its grapheme head. */
    if(s->snapshot[first].ch==RU_CONTINUATION&&first%t->cols)first--;
    for(i=first;i<=last;i++)if(cells[i].ch!=s->snapshot[i].ch){vt_selection_clear(s);return 1;}
    return 0;
}
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
    struct vt_cell *cells=s->snapshot?s->snapshot:vt_cells(t);char *text;
    unsigned first,last,at,end,i,n,k;unsigned long used=0;unsigned char encoded[4];
    *length=0;if(!s->active)return NULL;
    if(s->snapshot&&(s->cols!=t->cols||s->rows!=t->rows||s->alternate!=t->alternate))return NULL;
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
