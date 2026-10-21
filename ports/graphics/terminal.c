/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "terminal.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
struct vt_cell *vt_cells(struct vt *t){return t->cells[t->alternate];}
void vt_dirty(struct vt *t){memset(t->dirty,1,t->rows);}
static unsigned colour(struct vt *t) {
    unsigned f=t->fg,b=t->bg;
    if(t->bold&&f<8)f+=8;
    return (t->reverse?(f<<4)|b:(b<<4)|f) |
        (t->underline?256:0) | (t->blink?512:0);
}
static void clear_cell(struct vt *t,unsigned at) {
    struct vt_cell *p=vt_cells(t);
    if(p[at].ch==RU_CONTINUATION&&at%t->cols){p[at-1].ch=' ';}
    else if(p[at].ch>=128&&ru_glyph_width(p[at].ch)==2&&at%t->cols+1<t->cols)p[at+1].ch=' ';
    p[at].ch=' ';
}
/* Repair split glyphs after column insertion/deletion or a clipped resize. */
static void repair_row(struct vt_cell *p,unsigned cols) {
    unsigned x;int wide=0;
    for(x=0;x<cols;x++) {
        if(p[x].ch==RU_CONTINUATION) { if(!wide)p[x].ch=' ';wide=0; }
        else {
            wide=p[x].ch>=128&&ru_glyph_width(p[x].ch)==2;
            if(wide&&(x+1==cols||p[x+1].ch!=RU_CONTINUATION)){p[x].ch=RU_REPLACEMENT;wide=0;}
        }
    }
}
static void erase(struct vt *t,unsigned start,unsigned end) {
    struct vt_cell *p=vt_cells(t);unsigned i;
    if(end>t->rows*t->cols)end=t->rows*t->cols;
    if(start>=end)return;
    clear_cell(t,start);clear_cell(t,end-1);
    for(i=start;i<end;i++){p[i].ch=' ';p[i].colour=colour(t);t->dirty[i/t->cols]=1;}
}
static int valid_size(unsigned cols,unsigned rows) {
    return cols&&cols<=VT_MAX_COLS&&rows&&rows<=VT_MAX_ROWS;
}
void vt_destroy(struct vt *t) {
    free(t->cells[0]);free(t->cells[1]);memset(t,0,sizeof(*t));
}
int vt_init_size(struct vt *t,unsigned cols,unsigned rows) {
    struct vt_cell *a,*b;
    unsigned long size;
    if(!valid_size(cols,rows))return -1;
    size=(unsigned long)cols*rows*sizeof(*a);
    a=malloc(size);if(!a)return -1;
    b=malloc(size);if(!b){free(a);return -1;}
    memset(t,0,sizeof(*t));t->cells[0]=a;t->cells[1]=b;
    t->cols=cols;t->rows=rows;t->fg=7;t->bottom=rows-1;t->visible=t->wrap=1;
    erase(t,0,t->rows*t->cols);t->alternate=1;erase(t,0,t->rows*t->cols);t->alternate=0;
    return 0;
}
void vt_init(struct vt *t) {vt_init_size(t,VT_DEFAULT_COLS,VT_DEFAULT_ROWS);}
unsigned vt_mouse_report(struct vt *t,char *out,unsigned button,int x,int y,int released,int motion,unsigned modifiers) {
    unsigned code,n;
    if(!t->mouse_mode||!t->cols||!t->rows)return 0;
    if(motion&&(t->mouse_mode==1000||(t->mouse_mode==1002&&button==3)))return 0;
    if(button>67||(button>3&&button<64)||(released&&button>=64))return 0;
    if(x<0)x=0;if(y<0)y=0;
    if((unsigned)x>=t->cols)x=t->cols-1;if((unsigned)y>=t->rows)y=t->rows-1;
    code=button|(modifiers&28)|(motion?32:0);
    if(motion&&t->mouse_known&&t->mouse_x==(unsigned)x&&t->mouse_y==(unsigned)y&&t->mouse_code==code)return 0;
    if(t->mouse_sgr)n=snprintf(out,40,"\033[<%u;%u;%u%c",code,(unsigned)x+1,(unsigned)y+1,released?'m':'M');
    else {
        if(x>=223||y>=223)return 0;
        out[0]=27;out[1]='[';out[2]='M';out[3]=32+(released?3|(modifiers&28):code);
        out[4]=33+x;out[5]=33+y;out[6]=0;n=6;
    }
    if(button<3&&!motion){if(released)t->mouse_buttons&=~(1U<<button);else t->mouse_buttons|=1U<<button;}
    t->mouse_known=1;t->mouse_x=x;t->mouse_y=y;t->mouse_code=code;return n;
}
int vt_resize(struct vt *t,unsigned cols,unsigned rows) {
    struct vt_cell *next[2];
    unsigned plane,y,x,oldcols=t->cols,oldrows=t->rows,width,height;
    if(!valid_size(cols,rows)||!valid_size(oldcols,oldrows))return -1;
    if(cols==oldcols&&rows==oldrows)return 0;
    next[0]=malloc((unsigned long)cols*rows*sizeof(**next));
    if(!next[0])return -1;
    next[1]=malloc((unsigned long)cols*rows*sizeof(**next));
    if(!next[1]){free(next[0]);return -1;}
    width=cols<oldcols?cols:oldcols;height=rows<oldrows?rows:oldrows;
    for(plane=0;plane<2;plane++) {
        for(y=0;y<rows;y++)for(x=0;x<cols;x++) {
            next[plane][y*cols+x].ch=' ';next[plane][y*cols+x].colour=colour(t);
        }
        for(y=0;y<height;y++){memcpy(next[plane]+y*cols,t->cells[plane]+y*oldcols,width*sizeof(**next));repair_row(next[plane]+y*cols,cols);}
    }
    /* Publish only after both allocations succeed; failure preserves the model. */
    free(t->cells[0]);free(t->cells[1]);t->cells[0]=next[0];t->cells[1]=next[1];
    t->cols=cols;t->rows=rows;
    if(t->x>=cols)t->x=cols-1;if(t->y>=rows)t->y=rows-1;
    if(t->sx>=cols)t->sx=cols-1;if(t->sy>=rows)t->sy=rows-1;
    if(t->primary_x>=cols)t->primary_x=cols-1;if(t->primary_y>=rows)t->primary_y=rows-1;
    t->top=t->pending=t->last_at=t->grapheme.started=0;t->bottom=rows-1;
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
/* Approximate extended colours on the shared 16-colour ROM/ANSI palette.
 * Keep GUI consumers compatible with the existing packed colour cell ABI. */
static unsigned nearest(unsigned r,unsigned g,unsigned b) {
    static const unsigned char rgb[16][3]={{0,0,0},{170,0,0},{0,170,0},{170,85,0},
        {0,0,170},{170,0,170},{0,170,170},{170,170,170},
        {85,85,85},{255,85,85},{85,255,85},{255,255,85},
        {85,85,255},{255,85,255},{85,255,255},{255,255,255}};
    unsigned i,best=0;unsigned long distance=~0UL,d;long dr,dg,db;
    for(i=0;i<16;i++) {dr=(long)r-rgb[i][0];dg=(long)g-rgb[i][1];db=(long)b-rgb[i][2];
        d=dr*dr+dg*dg+db*db;if(d<distance){distance=d;best=i;}}
    return best;
}
static unsigned cube(unsigned c){return c?55+40*c:0;}
static unsigned indexed(unsigned c) {
    unsigned n;if(c<16)return c;
    if(c<232){n=c-16;return nearest(cube(n/36),cube((n/6)%6),cube(n%6));}
    n=8+(c-232)*10;return nearest(n,n,n);
}
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
            case 1000:case 1002:case 1003:
                if(on)t->mouse_mode=t->params[i];else if(t->mouse_mode==t->params[i])t->mouse_mode=0;
                t->mouse_known=t->mouse_buttons=0;break;
            case 1006:t->mouse_sgr=on;t->mouse_known=0;break;
            case 2004:t->bracketed_paste=on;break;
            case 2026:t->synchronized=on;break;
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
            if(!a){t->fg=7;t->bg=t->bold=t->reverse=t->underline=t->blink=0;}
            else if(a==1)t->bold=1;else if(a==22)t->bold=0;
            else if(a==4)t->underline=1;else if(a==24)t->underline=0;
            else if(a==5)t->blink=1;else if(a==25)t->blink=0;
            else if(a==7)t->reverse=1;else if(a==27)t->reverse=0;
            else if(a>=30&&a<=37)t->fg=a-30;else if(a==39)t->fg=7;
            else if(a>=40&&a<=47)t->bg=a-40;else if(a==49)t->bg=0;
            else if(a>=90&&a<=97)t->fg=a-90+8;
            else if(a>=100&&a<=107)t->bg=a-100+8;
            else if(a==38||a==48) {
                unsigned mapped=7,valid=0;
                if(i+2<=t->count&&t->params[i+1]==5){
                    if(t->params[i+2]<256){mapped=indexed(t->params[i+2]);valid=1;}i+=2;
                } else if(i+4<=t->count&&t->params[i+1]==2){
                    if(t->params[i+2]<256&&t->params[i+3]<256&&t->params[i+4]<256){mapped=nearest(t->params[i+2],t->params[i+3],t->params[i+4]);valid=1;}i+=4;
                }
                if(valid){if(a==38)t->fg=mapped;else t->bg=mapped;}
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
        repair_row(p+t->y*t->cols,t->cols);t->dirty[t->y]=1;break;
    case 'X':erase(t,start,start+bound(n,t->cols-t->x));break;
    case 'q':if(t->intermediate==' '&&t->params[0]<=6)t->cursor_shape=t->params[0];break;
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
static void put_cell(struct vt *t,uint32_t cp) {
    struct vt_cell *p=vt_cells(t);unsigned at,w=1,old;int width;
    if(cp>=128||(t->grapheme.started&&t->grapheme.previous==RU_PREPEND)) {
        if(!t->last_at){t->grapheme.started=0;ru_join(&t->grapheme,cp);}
        if(t->last_at&&!t->grapheme.started){
            unsigned i,n=ru_scalar_count(p[t->last_at-1].ch);
            for(i=0;i<n;i++)ru_join(&t->grapheme,ru_scalar(p[t->last_at-1].ch,i));
        }
        if(t->last_at&&ru_join(&t->grapheme,cp)) {
            uint32_t glyph;at=t->last_at-1;old=ru_glyph_width(p[at].ch);
            glyph=ru_append(p[at].ch,cp);w=ru_glyph_width(glyph);
            if(w==2&&at%t->cols+1==t->cols){glyph=RU_REPLACEMENT;w=1;}
            p[at].ch=glyph;
            if(w<old){p[at+1].ch=' ';if(t->pending)t->pending=0;else if(t->x)--t->x;}
            if(w>old){clear_cell(t,at+1);p[at+1].ch=RU_CONTINUATION;p[at+1].colour=p[at].colour;
                if(t->x+1<t->cols)++t->x;else t->pending=t->wrap;}
            t->dirty[at/t->cols]=1;return;
        }
        width=ru_width(cp);if(width<0)return;
        if(!width){cp=ru_append(0x25cc,cp);w=1;}else w=width;
    } else t->grapheme.started=0;
    if(t->pending){t->x=0;newline(t);}
    if(w==2&&(t->cols<2||t->x+1==t->cols)){
        if(t->wrap&&t->cols>1){t->x=0;newline(t);}else{cp=RU_REPLACEMENT;w=1;}
    }
    at=t->y*t->cols+t->x;
    if(p[at].ch>=128)clear_cell(t,at);
    if(w==2)clear_cell(t,at+1);
    p[at].ch=cp;p[at].colour=colour(t);
    if(w==2){p[at+1].ch=RU_CONTINUATION;p[at+1].colour=p[at].colour;}
    t->last_at=at+1;t->dirty[t->y]=1;
    if(t->x+w<t->cols)t->x+=w;else{t->x=t->cols-1;t->pending=t->wrap;}
}
void vt_feed(struct vt *t,const char *s,unsigned size) {
    unsigned c;
    t->dirty[t->y]=1;
    while(size--) {
        c=(unsigned char)*s++;
        /* Most terminal output is a printable ASCII run. Cache style and
         * cursor state once, and avoid Unicode lookup/decoder calls entirely. */
        if(!t->parser&&!t->utf8.need&&!t->pending&&c>=32&&c<127&&
           !(t->shift?t->g1:t->g0)&&!(t->grapheme.started&&t->grapheme.previous==RU_PREPEND)) {
            struct vt_cell *cells=vt_cells(t);unsigned x=t->x,at=t->y*t->cols+x,attr=colour(t);
            do {
                if(cells[at].ch>=128)clear_cell(t,at);
                cells[at].ch=c;cells[at].colour=attr;++at;++x;
                if(x==t->cols||!size||(unsigned char)*s<32||(unsigned char)*s>=127)break;
                c=(unsigned char)*s++;--size;
            }while(1);
            t->last_at=at;t->grapheme.started=0;t->dirty[t->y]=1;
            if(x==t->cols){t->x=x-1;t->pending=t->wrap;}else t->x=x;
            continue;
        }
        if(!t->parser&&(t->utf8.need||c>=128)) {
            uint32_t cp;int decoded=ru_decode(&t->utf8,c,&cp);
            if(decoded)put_cell(t,cp);
            if(decoded>=0)continue;
            /* The rejected octet may itself start another multibyte scalar. */
            if(c>=128){decoded=ru_decode(&t->utf8,c,&cp);if(decoded)put_cell(t,cp);continue;}
            /* Otherwise reprocess the rejected ASCII/control/ESC below. */
        }
        if(c<32||t->parser){t->last_at=0;t->grapheme.started=0;}
        if(t->parser==3){
            if(c==7){if(t->osc_active&&t->osc)t->osc(t,0,1);t->osc_active=t->parser=0;}
            else if(c==24||c==26){if(t->osc_active&&t->osc)t->osc(t,0,-1);t->osc_active=t->parser=0;}
            else if(c==27)t->parser=4;
            else if(t->osc_active&&t->osc)t->osc(t,c,0);
            continue;
        }
        if(t->parser==4){
            if(t->osc_active){if(t->osc)t->osc(t,0,c=='\\'?1:-1);t->osc_active=0;}
            t->parser=c=='\\'?0:3;continue;
        }
        if(t->parser==6){if(c==27)t->parser=4;continue;}
        if(t->parser==5){if(t->charset_target)t->g1=c=='0';else t->g0=c=='0';t->parser=0;continue;}
        if(t->parser==1) {
            t->parser=0;
            switch(c) {
            case '[':t->parser=2;t->count=t->private_mode=t->intermediate=0;memset(t->params,0,sizeof(t->params));break;
            case ']':t->parser=3;t->osc_active=1;if(t->osc)t->osc(t,0,-1);break;
            case 'P':case '^':case '_':t->parser=6;break;
            case '(':case ')':t->charset_target=c==')';t->parser=5;break;
            case '7':t->sx=t->x;t->sy=t->y;break;
            case '8':t->x=t->sx;t->y=t->sy;t->pending=0;break;
            case 'D':newline(t);break;
            case 'E':newline(t);t->x=0;break;
            case 'M':if(t->y==t->top)scroll(t,-1);else if(t->y)--t->y;break;
            case 'c':
                t->x=t->y=t->top=t->pending=t->alternate=t->appcursor=t->bracketed_paste=0;
                t->mouse_mode=t->mouse_sgr=t->mouse_buttons=t->mouse_known=0;
                t->bottom=t->rows-1;t->fg=7;t->bg=t->bold=t->reverse=t->underline=t->blink=0;t->visible=t->wrap=1;
                t->g0=t->g1=t->shift=0;
                erase(t,0,t->rows*t->cols);break;
            }
            continue;
        }
        if(t->parser==2) {
            if(c=='?'&&!t->count&&!t->params[0])t->private_mode=1;
            else if(c>='0'&&c<='9')t->params[t->count]=(t->params[t->count]>999?9999:t->params[t->count]*10+c-'0');
            else if(c==';'){if(t->count<15)++t->count;else t->parser=0;}
            else if(c>=32&&c<=47)t->intermediate=c;
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
        /* DEC Special Graphics and UTF-8 address the same Unicode glyphs. */
        if(t->shift?t->g1:t->g0) {
            switch(c) {
            case 'q':c=0x2500;break;case 'x':c=0x2502;break;
            case 'l':c=0x250c;break;case 'k':c=0x2510;break;
            case 'm':c=0x2514;break;case 'j':c=0x2518;break;
            case 't':c=0x251c;break;case 'u':c=0x2524;break;
            case 'w':c=0x252c;break;case 'v':c=0x2534;break;
            case 'n':c=0x253c;break;
            }
        }
        put_cell(t,c);
    }
    t->dirty[t->y]=1;
}
