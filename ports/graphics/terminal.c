/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "terminal.h"
#include <string.h>
#include <stdio.h>
struct vt_cell *vt_cells(struct vt *t){return t->cells[t->alternate];}
void vt_dirty(struct vt *t){memset(t->dirty,1,t->rows);}
static unsigned colour(struct vt *t) {
    unsigned f=t->fg,b=t->bg;
    if(t->bold&&f<8)f+=8;
    return t->reverse?(f<<4)|b:(b<<4)|f;
}
static void erase(struct vt *t,unsigned start,unsigned end) {
    struct vt_cell *p=vt_cells(t);unsigned i;
    if(end>t->rows*t->cols)end=t->rows*t->cols;
    for(i=start;i<end;i++){p[i].ch=' ';p[i].colour=colour(t);t->dirty[i/t->cols]=1;}
}
static int valid_size(unsigned cols,unsigned rows) {
    return cols&&cols<=VT_MAX_COLS&&rows&&rows<=VT_MAX_ROWS;
}
int vt_init_size(struct vt *t,unsigned cols,unsigned rows) {
    if(!valid_size(cols,rows))return -1;
    memset(t,0,sizeof(*t));t->cols=cols;t->rows=rows;t->fg=7;t->bottom=rows-1;t->visible=t->wrap=1;
    erase(t,0,t->rows*t->cols);t->alternate=1;erase(t,0,t->rows*t->cols);t->alternate=0;
    return 0;
}
void vt_init(struct vt *t) {vt_init_size(t,VT_DEFAULT_COLS,VT_DEFAULT_ROWS);}
int vt_resize(struct vt *t,unsigned cols,unsigned rows) {
    unsigned plane,y,x,oldcols=t->cols,oldrows=t->rows,width,height;
    if(!valid_size(cols,rows)||!valid_size(oldcols,oldrows))return -1;
    if(cols==oldcols&&rows==oldrows)return 0;
    width=cols<oldcols?cols:oldcols;height=rows<oldrows?rows:oldrows;
    for(plane=0;plane<2;plane++) {
        struct vt_cell *p=t->cells[plane];
        /* Widen bottom-up and narrow top-down so restriding cannot overwrite
         * a source row that has not been moved yet. */
        if(cols>oldcols) {
            y=height;
            while(y--){memmove(p+y*cols,p+y*oldcols,width*sizeof(*p));}
        } else for(y=0;y<height;y++)memmove(p+y*cols,p+y*oldcols,width*sizeof(*p));
        for(y=0;y<rows;y++)for(x=0;x<cols;x++)if(y>=oldrows||x>=oldcols) {
            p[y*cols+x].ch=' ';p[y*cols+x].colour=colour(t);
        }
    }
    t->cols=cols;t->rows=rows;
    if(t->x>=cols)t->x=cols-1;if(t->y>=rows)t->y=rows-1;
    if(t->sx>=cols)t->sx=cols-1;if(t->sy>=rows)t->sy=rows-1;
    if(t->primary_x>=cols)t->primary_x=cols-1;if(t->primary_y>=rows)t->primary_y=rows-1;
    t->top=t->pending=0;t->bottom=rows-1;
    memset(t->dirty,0,sizeof(t->dirty));vt_dirty(t);return 0;
}
static void scroll(struct vt *t,int amount) {
    unsigned n=amount<0?-amount:amount,rows=t->bottom-t->top+1,i;
    struct vt_cell *p=vt_cells(t)+t->top*t->cols;
    if(n>rows)n=rows;
    if(t->scroll)t->scroll(t,amount<0?-(int)n:(int)n);
    if(amount>0) {
        memmove(p,p+n*t->cols,(unsigned long)(rows-n)*t->cols*sizeof(*p));
        erase(t,(t->bottom+1-n)*t->cols,(t->bottom+1)*t->cols);
    } else {
        memmove(p+n*t->cols,p,(unsigned long)(rows-n)*t->cols*sizeof(*p));
        erase(t,t->top*t->cols,(t->top+n)*t->cols);
    }
    if(!t->scroll)for(i=t->top;i<=t->bottom;i++)t->dirty[i]=1;
}
static void newline(struct vt *t) {
    t->pending=0;
    if(t->y==t->bottom)scroll(t,1);else if(t->y<t->rows-1)++t->y;
}
static unsigned param(struct vt *t,unsigned n,unsigned fallback) {
    return n<=t->count&&t->params[n]?t->params[n]:fallback;
}
static unsigned bound(unsigned n,unsigned max){return n>max?max:n;}
static void csi(struct vt *t,unsigned c) {
    unsigned n=param(t,0,1),i,start=t->y*t->cols+t->x;
    struct vt_cell *p=vt_cells(t);
    if(t->private_mode) {
        for(i=0;i<=t->count;i++) {
            unsigned on=c=='h';
            if(c!='h'&&c!='l')continue;
            switch(t->params[i]) {
            case 1:t->appcursor=on;break;
            case 7:t->wrap=on;t->pending=0;break;
            case 25:t->visible=on;break;
            case 47:case 1047:case 1049:
                if(on&&!t->alternate) {
                    t->primary_x=t->x;t->primary_y=t->y;t->alternate=1;t->x=t->y=0;
                    erase(t,0,t->rows*t->cols);
                } else if(!on&&t->alternate){t->alternate=0;t->x=t->primary_x;t->y=t->primary_y;}
                t->top=0;t->bottom=t->rows-1;t->pending=0;vt_dirty(t);break;
            }
        }
        return;
    }
    switch(c) {
    case 'A':t->y=n>t->y?0:t->y-n;break;
    case 'B':t->y=bound(t->y+n,t->rows-1);break;
    case 'C':t->x=bound(t->x+n,t->cols-1);break;
    case 'D':t->x=n>t->x?0:t->x-n;break;
    case 'E':t->y=bound(t->y+n,t->rows-1);t->x=0;break;
    case 'F':t->y=n>t->y?0:t->y-n;t->x=0;break;
    case 'G':case '`':t->x=bound(n-1,t->cols-1);break;
    case 'd':t->y=bound(n-1,t->rows-1);break;
    case 'H':case 'f':t->y=bound(n-1,t->rows-1);t->x=bound(param(t,1,1)-1,t->cols-1);break;
    case 'J':
        if(t->params[0]==0)erase(t,start,t->rows*t->cols);
        else if(t->params[0]==1)erase(t,0,start+1);
        else if(t->params[0]==2||t->params[0]==3)erase(t,0,t->rows*t->cols);
        break;
    case 'K':
        if(t->params[0]==0)erase(t,start,(t->y+1)*t->cols);
        else if(t->params[0]==1)erase(t,t->y*t->cols,start+1);
        else if(t->params[0]==2)erase(t,t->y*t->cols,(t->y+1)*t->cols);
        break;
    case 'm':
        for(i=0;i<=t->count;i++) {
            unsigned a=t->params[i];
            if(!a){t->fg=7;t->bg=t->bold=t->reverse=0;}
            else if(a==1)t->bold=1;else if(a==22)t->bold=0;
            else if(a==7)t->reverse=1;else if(a==27)t->reverse=0;
            else if(a>=30&&a<=37)t->fg=a-30;else if(a==39)t->fg=7;
            else if(a>=40&&a<=47)t->bg=a-40;else if(a==49)t->bg=0;
            else if(a>=90&&a<=97)t->fg=a-90+8;
            else if(a>=100&&a<=107)t->bg=a-100+8;
            else if(a==38||a==48) { /* consume unsupported extended colour payload */
                if(i+1<=t->count&&t->params[i+1]==5)i=bound(i+2,t->count);
                else if(i+1<=t->count&&t->params[i+1]==2)i=bound(i+4,t->count);
            }
        }
        return;
    case 'r':
        i=param(t,1,t->rows);
        if(n<i&&i<=t->rows){t->top=n-1;t->bottom=i-1;t->x=t->y=0;}
        break;
    case 'S':scroll(t,(int)bound(n,t->rows));break;
    case 'T':scroll(t,-(int)bound(n,t->rows));break;
    case 'L':case 'M':
        if(t->y>=t->top&&t->y<=t->bottom) {
            i=t->top;t->top=t->y;scroll(t,c=='M'?(int)bound(n,t->rows):-(int)bound(n,t->rows));t->top=i;
        }
        break;
    case '@':case 'P':
        n=bound(n,t->cols-t->x);
        if(c=='@'){memmove(p+start+n,p+start,(t->cols-t->x-n)*sizeof(*p));erase(t,start,start+n);}
        else {memmove(p+start,p+start+n,(t->cols-t->x-n)*sizeof(*p));erase(t,(t->y+1)*t->cols-n,(t->y+1)*t->cols);}
        t->dirty[t->y]=1;break;
    case 'X':erase(t,start,start+bound(n,t->cols-t->x));break;
    case 's':t->sx=t->x;t->sy=t->y;break;
    case 'u':t->x=t->sx;t->y=t->sy;break;
    case 'n':
        if(t->reply) {
            char response[32];
            if(n==6){snprintf(response,sizeof(response),"\033[%u;%uR",t->y+1,t->x+1);t->reply(t,response);}
            else if(n==5)t->reply(t,"\033[0n");
        }
        return;
    case 'c':if(t->reply)t->reply(t,"\033[?1;2c");return;
    }
    t->pending=0;
}
void vt_feed(struct vt *t,const char *s,unsigned size) {
    unsigned c;
    t->dirty[t->y]=1;
    while(size--) {
        c=(unsigned char)*s++;
        if(t->parser==3){if(c==7)t->parser=0;else if(c==27)t->parser=4;continue;}
        if(t->parser==4){t->parser=c=='\\'?0:3;continue;}
        if(t->parser==5){if(t->charset_target)t->g1=c=='0';else t->g0=c=='0';t->parser=0;continue;}
        if(t->parser==1) {
            t->parser=0;
            switch(c) {
            case '[':t->parser=2;t->count=t->private_mode=0;memset(t->params,0,sizeof(t->params));break;
            case ']':t->parser=3;break;
            case '(':case ')':t->charset_target=c==')';t->parser=5;break;
            case '7':t->sx=t->x;t->sy=t->y;break;
            case '8':t->x=t->sx;t->y=t->sy;t->pending=0;break;
            case 'D':newline(t);break;
            case 'E':newline(t);t->x=0;break;
            case 'M':if(t->y==t->top)scroll(t,-1);else if(t->y)--t->y;break;
            case 'c':
                t->x=t->y=t->top=t->pending=t->alternate=t->appcursor=0;
                t->bottom=t->rows-1;t->fg=7;t->bg=t->bold=t->reverse=0;t->visible=t->wrap=1;
                t->g0=t->g1=t->shift=0;
                erase(t,0,t->rows*t->cols);break;
            }
            continue;
        }
        if(t->parser==2) {
            if(c=='?'&&!t->count&&!t->params[0])t->private_mode=1;
            else if(c>='0'&&c<='9')t->params[t->count]=(t->params[t->count]>999?9999:t->params[t->count]*10+c-'0');
            else if(c==';'){if(t->count<15)++t->count;else t->parser=0;}
            else if(c>=64&&c<=126){csi(t,c);t->parser=0;}
            else if(c==27)t->parser=1;else t->parser=0;
            continue;
        }
        if(c==27){t->parser=1;continue;}
        if(c==14||c==15){t->shift=c==14;continue;}
        if(c=='\r'){t->x=t->pending=0;continue;}
        if(c=='\n'||c==11||c==12){newline(t);continue;}
        if(c==8){if(t->x)--t->x;t->pending=0;continue;}
        if(c==9){t->x=bound((t->x/8+1)*8,t->cols-1);t->pending=0;continue;}
        if(c<32||c==127)continue;
        if(t->pending){t->x=0;newline(t);}
        /* DEC Special Graphics maps onto the ROM font's CP437 line cells. */
        if(t->shift?t->g1:t->g0) {
            switch(c) {
            case 'q':c=196;break;case 'x':c=179;break;
            case 'l':c=218;break;case 'k':c=191;break;
            case 'm':c=192;break;case 'j':c=217;break;
            case 't':c=195;break;case 'u':c=180;break;
            case 'w':c=194;break;case 'v':c=193;break;
            case 'n':c=197;break;
            }
        }
        vt_cells(t)[t->y*t->cols+t->x].ch=c<256?c:'?';
        vt_cells(t)[t->y*t->cols+t->x].colour=colour(t);t->dirty[t->y]=1;
        if(t->x<t->cols-1)++t->x;else t->pending=t->wrap;
    }
    t->dirty[t->y]=1;
}
