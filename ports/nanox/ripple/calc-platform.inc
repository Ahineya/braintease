/* SPDX-License-Identifier: GPL-2.0-or-later */
/* The selected compiled font has no kerning. Measure it once, not per button. */
static GR_FONT_INFO calc_font;
static int label_width[ROWS*COLS],pressed=-1;
static int calc_width(const char *s){int width=0;while(*s){width+=calc_font.fixed?calc_font.maxwidth:calc_font.widths[(unsigned char)*s];s++;}return width;}
static int button_at(int x,int y){int i;GR_RECT *r;for(i=0;i<ROWS*COLS;i++){r=&btn_rects[i/COLS][i%COLS];if(btn_labels[i]&&x>=r->x&&x<r->x+r->width&&y>=r->y&&y<r->y+r->height)return i;}return -1;}
static void draw_display(void){
    int x=WIN_W-12-calc_width(display);if(x<10)x=10;
    GrFillRect(win,gc_button,5,5,WIN_W-10,DISP_H-10);
    GrRect(win,gc_border,5,5,WIN_W-11,DISP_H-10);
    GrText(win,gc_text,x,5+DISP_H/2-10,display,strlen(display),GR_TFTOP);
}
static void draw_button(int i,int down){
    GR_RECT *r=&btn_rects[i/COLS][i%COLS];
    GrFillRect(win,down?gc_button_press:gc_button,r->x,r->y,r->width,r->height);
    GrRect(win,gc_border,r->x,r->y,r->width,r->height);
    if(btn_labels[i])GrText(win,gc_text,r->x+r->width/2-label_width[i]/2,r->y+r->height/2-6,btn_labels[i],strlen(btn_labels[i]),GR_TFTOP);
}
