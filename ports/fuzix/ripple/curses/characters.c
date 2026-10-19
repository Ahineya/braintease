/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <curses.h>
#include <string.h>
#include "curspriv.h"
static void changed(WINDOW *w,int y,int x) {
    if(w->_minchng[y]==_NO_CHANGE||x<w->_minchng[y])w->_minchng[y]=x;
    if(w->_maxchng[y]<x)w->_maxchng[y]=x;
}
static void write_cell(WINDOW *w,int y,int x,chtype cell) {
    if(w->_line[y][x]!=cell){w->_line[y][x]=cell;changed(w,y,x);}
}
static void clear_half(WINDOW *w,int y,int x) {
    uint32_t old=w->_line[y][x]&A_CHARTEXT;
    if(old==RU_CONTINUATION&&x)write_cell(w,y,x-1,' '|(w->_line[y][x-1]&A_ATTRIBUTES));
    else if(old>=128&&ru_glyph_width(old)==2&&x<w->_maxx)write_cell(w,y,x+1,' '|(w->_line[y][x+1]&A_ATTRIBUTES));
}
static int advance_line(WINDOW *w) {
    w->_curx=0;
    if(w->_cury<w->_regbottom){++w->_cury;return OK;}
    if(!w->_scroll)return ERR;
    return wscrl(w,1);
}
static int scalar(WINDOW *w,uint32_t cp,int clip,chtype attrs) {
    int x,y,width=1,oldwidth;chtype cell;uint32_t glyph;
    if(!w)return ERR;
    if(cp>0x10ffffUL||(cp>=0xd800&&cp<=0xdfff))cp=RU_REPLACEMENT;
    if(cp>=128||(w->_grapheme.started&&w->_grapheme.previous==RU_PREPEND)) {
        if(!w->_last_valid){w->_grapheme.started=0;ru_join(&w->_grapheme,cp);}
        if(w->_last_valid&&!w->_grapheme.started){
            unsigned i,n=ru_scalar_count(w->_line[w->_lasty][w->_lastx]&A_CHARTEXT);
            for(i=0;i<n;i++)ru_join(&w->_grapheme,ru_scalar(w->_line[w->_lasty][w->_lastx]&A_CHARTEXT,i));
        }
        if(w->_last_valid&&ru_join(&w->_grapheme,cp)){
            x=w->_lastx;y=w->_lasty;cell=w->_line[y][x];oldwidth=ru_glyph_width(cell&A_CHARTEXT);
            glyph=ru_append(cell&A_CHARTEXT,cp);width=ru_glyph_width(glyph);
            if(width==2&&x==w->_maxx){
                if(clip){write_cell(w,y,x,(cell&A_ATTRIBUTES)|' ');w->_last_valid=0;return ERR;}
                glyph=RU_REPLACEMENT;width=1;
            }
            write_cell(w,y,x,(cell&A_ATTRIBUTES)|glyph);
            if(width<oldwidth){write_cell(w,y,x+1,' '|(cell&A_ATTRIBUTES));if(w->_curx>0)--w->_curx;}
            if(width>oldwidth){clear_half(w,y,x+1);write_cell(w,y,x+1,(cell&A_ATTRIBUTES)|RU_CONTINUATION);++w->_curx;}
            return OK;
        }
        width=ru_width(cp);if(width<0){cp=RU_REPLACEMENT;width=1;}
        if(width<=0){cp=ru_append(0x25cc,cp);width=1;}
    }else w->_grapheme.started=0;
    if(cp<32||cp==127)cp='?';
    if(w->_cury<0||w->_cury>w->_maxy||w->_curx<0)return ERR;
    if(w->_curx+width>w->_maxx+1){
        if(clip)return ERR;
        if(advance_line(w)==ERR)return ERR;
    }
    if(width==2&&w->_maxx==0){cp=RU_REPLACEMENT;width=1;}
    x=w->_curx;y=w->_cury;if(x>w->_maxx)return ERR;
    clear_half(w,y,x);if(width==2)clear_half(w,y,x+1);
    write_cell(w,y,x,(attrs&A_ATTRIBUTES)|cp);
    if(width==2)write_cell(w,y,x+1,(attrs&A_ATTRIBUTES)|RU_CONTINUATION);
    w->_last_valid=1;w->_lastx=x;w->_lasty=y;w->_curx+=width;
    if(!clip&&w->_curx>w->_maxx){int status=advance_line(w);if(status==ERR){w->_curx=w->_maxx;return ERR;}}
    return OK;
}
int ripple_curses_scalar(WINDOW *w,uint32_t cp){return scalar(w,cp,1,w?w->_attrs:0);}
int waddch(WINDOW *w,chtype c) {
    uint32_t cp=c&A_CHARTEXT;int stop;
    if(!w)return ERR;
    if(cp=='\t'){stop=((w->_curx/w->_tabsize)+1)*w->_tabsize;do{if(waddch(w,' ')==ERR)return ERR;}while(w->_curx&&w->_curx<stop);return OK;}
    if(cp=='\n'){w->_last_valid=0;return advance_line(w);}
    if(cp=='\r'){w->_last_valid=0;w->_curx=0;return OK;}
    if(cp=='\b'){w->_last_valid=0;if(w->_curx>0)--w->_curx;return OK;}
    if(cp<32||cp==127){if(waddch(w,'^')==ERR)return ERR;return waddch(w,cp==127?'?':cp+'@');}
    return scalar(w,cp,0,(c&A_ATTRIBUTES)|w->_attrs);
}
int wadd_utf8(WINDOW *w,const unsigned char *p,size_t n) {
    size_t at=0;uint32_t cp;
    while(at<n){cp=ru_next(p,n,&at);if(waddch(w,cp)==ERR)return ERR;}return OK;
}
int waddstr(WINDOW *w,char *text){return wadd_utf8(w,(const unsigned char *)text,strlen(text));}
