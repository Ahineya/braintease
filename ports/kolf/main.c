/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Native Nano-X frontend. The course and simulation modules have no GUI I/O. */
#include "golf.h"
#include "putting.h"
#include "dial.inc"
#include "courses.inc"
#include "../../libraries/deflate/deflate.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <errno.h>
#include <sys/stat.h>
#include "nano-X.h"
#define WIDTH 610
#define HEIGHT 430
#define BX 10
#define BY 24
#define SX 428
#define PI 3.14159265358979323846
static GolfCourse *course;
static GolfGame game;
static GR_WINDOW_ID window,background,ground,canvas,dial_face;
static GR_GC_ID gc;
static GR_REGION_ID board_region;
static int hole,dirty=1,hud_dirty=1,help,choosing,choice,selected_course,paused,trace,connected;
static GolfPutt putt;
static int mouse_held,space_held,down_held,owner,guide=1;
static int water_choice;
static int scores[GOLF_HOLES],laststate=-1,laststrokes=-1;
static unsigned long lastrevision=~0UL;
static int aim_x,aim_y;
static int oldx,oldy,oldw,oldh;
static double angle,undo_angle;
static volatile int stopping;
static int wake_pipe[2]={-1,-1};
static unsigned long next_tick,next_frame,frames,steps;
static char message[80];
static void cancel_swing(void){putt_cancel(&putt);owner=0;hud_dirty=1;}
static void swing_update(unsigned long now);
static int swinging(void){return putt.phase!=PUTT_IDLE&&putt.phase!=PUTT_FIRE;}
static unsigned long milliseconds(void) {
    struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);
    return t.tv_sec*1000UL+t.tv_nsec/1000000UL;
}
static void colour(GR_COLOR c){GrSetGCForeground(gc,c);}
static void box(GR_DRAW_ID d,int x,int y,int w,int h,GR_COLOR c) {
    if(w<=0||h<=0)return;colour(c);GrFillRect(d,gc,x,y,w,h);
}
static void text(int x,int y,const char *s,GR_COLOR c) {
    colour(c);GrText(window,gc,x,y,(void *)s,-1,GR_TFASCII|GR_TFTOP);
}
static unsigned long root(unsigned long n) {
    unsigned long bit=1UL<<30,out=0;
    while(bit>n)bit>>=2;
    while(bit){if(n>=out+bit){n-=out+bit;out=(out>>1)+bit;}else out>>=1;bit>>=2;}
    return out;
}
static int ellipse_span(int rx,int ry,int yy) {
    unsigned long r2,inside;
    if(rx<1||ry<1||yy < -ry||yy>ry)return 0;
    r2=(long)ry*ry;inside=r2-(long)yy*yy;
    return (int)root((unsigned long)((unsigned long long)rx*rx*inside/r2));
}
/* Rasterization stays integer-only: no math-service call per painted row. */
static void ellipse(GR_DRAW_ID d,int cx,int cy,int rx,int ry,GR_COLOR c) {
    int y,x,lo=cy-ry,hi=cy+ry;
    if(rx<1||ry<1)return;
    if(d==background||d==ground){if(lo<0)lo=0;if(hi>399)hi=399;}
    colour(c);
    for(y=lo;y<=hi;y++){
        x=ellipse_span(rx,ry,y-cy);
        GrFillRect(d,gc,cx-x,y,2*x+1,1);
    }
}
static void line(GR_DRAW_ID d,int x,int y,int xx,int yy,GR_COLOR c) {
    colour(c);GrLine(d,gc,x,y,xx,yy);
}
static void arrow(GR_DRAW_ID d,int x,int y,double dx,double dy,GR_COLOR c) {
    int xx=x+(int)(dx*14),yy=y+(int)(dy*14);
    line(d,x,y,xx,yy,c);
    line(d,xx,yy,xx-(int)(dx*5-dy*4),yy-(int)(dy*5+dx*4),c);
    line(d,xx,yy,xx-(int)(dx*5+dy*4),yy-(int)(dy*5-dx*4),c);
}
static void terrain(const GolfObject *o) {
    int y,left,right,lo,hi,cx,cy,x0=(int)o->x,y0=(int)o->y,ww=(int)o->w,hh=(int)o->h;
    double dx,dy,len;GR_COLOR c;
    if(o->type==G_SLOPE){
        lo=y0;hi=y0+hh;if(lo<0)lo=0;if(hi>399)hi=399;
        for(y=lo;y<=hi;y++){
            left=x0;right=x0+ww;
            if(o->gradient==G_DIAGONAL){cx=x0+ww-(long)ww*(y-y0)/hh;if(o->reversed)right=cx;else left=cx;}
            if(o->gradient==G_CROSS){cx=x0+(long)ww*(y-y0)/hh;if(o->reversed)left=cx;else right=cx;}
            if(o->gradient==G_ELLIPTIC){cx=x0+ww/2;cy=y0+hh/2;right=ellipse_span(ww/2,hh/2,y-cy);left=cx-right;right+=cx;}
            if(left<0)left=0;if(right>399)right=399;
            c=(y/12)&1?GR_RGB(63,126,84):GR_RGB(59,120,80);
            box(canvas,left,y,right-left+1,1,c);
        }
        cx=(int)(o->x+o->w/2);cy=(int)(o->y+o->h/2);
        if(o->gradient==G_DIAGONAL){cx=(int)(o->x+o->w*(o->reversed?.25:.75));cy=(int)(o->y+o->h*(o->reversed?.25:.75));}
        if(o->gradient==G_CROSS){cx=(int)(o->x+o->w*(o->reversed?.75:.25));cy=(int)(o->y+o->h*(o->reversed?.25:.75));}
        dx=o->gradient==G_VERTICAL?0:1;dy=o->gradient==G_HORIZONTAL?0:1;
        if(o->gradient==G_DIAGONAL||o->gradient==G_CROSS){len=sqrt(o->w*o->w+o->h*o->h);dx=o->h/len;dy=o->w/len;if(o->gradient==G_CROSS)dx=-dx;}
        if(!o->reversed){dx=-dx;dy=-dy;}
        if(cx>16&&cx<384&&cy>16&&cy<384){
            if(o->gradient==G_ELLIPTIC){
                for(y=0;y<4;y++)arrow(canvas,cx,cy,cos(y*PI/2)*(o->reversed?1:-1),sin(y*PI/2)*(o->reversed?1:-1),GR_RGB(159,194,136));
            }else arrow(canvas,cx,cy,dx,dy,GR_RGB(159,194,136));
        }
    }else if(o->type==G_SAND||o->type==G_WATER){
        c=o->type==G_SAND?GR_RGB(194,166,106):GR_RGB(53,116,150);
        ellipse(canvas,(int)o->x,(int)o->y,(int)o->w/2,(int)o->h/2,c);
        /* Sparse surface marks show the hazard without texture uploads. */
        cx=(int)o->x;cy=(int)o->y;
        for(y=8;y<400;y+=20)if(abs(y-cy)<=(int)o->h/2){
            right=ellipse_span((int)o->w/2,(int)o->h/2,y-cy);
            for(left=8;left<400;left+=24)if(left>=cx-right&&left+7<=cx+right)
                box(canvas,left,y,o->type==G_SAND?2:7,1,o->type==G_SAND?GR_RGB(161,134,81):GR_RGB(88,154,180));
        }
    }else if(o->type==G_CUP){
        ellipse(canvas,(int)o->x,(int)o->y+1,9,8,GR_RGB(97,158,104));
        ellipse(canvas,(int)o->x,(int)o->y,8,8,GR_RGB(13,32,28));
        line(canvas,(int)o->x+7,(int)o->y-3,(int)o->x+7,(int)o->y-24,GR_RGB(224,223,191));
        box(canvas,(int)o->x+8,(int)o->y-24,10,6,GR_RGB(223,174,82));
    }else if(o->type==G_WALL){
        for(y=-2;y<=2;y++)line(canvas,(int)o->ax,(int)o->ay+y,(int)o->bx,(int)o->by+y,GR_RGB(84,62,48));
        line(canvas,(int)o->ax,(int)o->ay-2,(int)o->bx,(int)o->by-2,GR_RGB(176,147,105));
    }else if(golf_rectangle(o->type)){
        c=o->type==G_SIGN?GR_RGB(113,88,56):o->type==G_FLOATER?GR_RGB(135,112,76):GR_RGB(147,132,96);
        box(canvas,x0,y0,ww,hh,c);
        for(y=y0+4;y<y0+hh&&y<400;y+=8)if(y>=0)line(canvas,x0,y,x0+ww,y,GR_RGB(104,90,65));
        if(o->type==G_WINDMILL){
            box(canvas,x0+ww/2-6,y0+hh/2-6,12,12,GR_RGB(77,86,85));
        }
        if(o->type==G_SIGN&&o->comment){
            const char *p=o->comment;char row[64];int at=0,col=ww/8-2,yy=y0+4,tag=0;
            if(col>60)col=60;
            while(*p&&yy<y0+hh-12&&col>0){
                int ch=(unsigned char)*p++;
                if(ch=='<'){tag=1;continue;}if(ch=='>'){tag=0;continue;}if(tag)continue;
                if(ch=='\n'||at==col){row[at]=0;colour(GR_RGB(248,236,202));GrText(canvas,gc,x0+6,yy,row,-1,GR_TFASCII|GR_TFTOP);at=0;yy+=14;if(ch=='\n')continue;}
                row[at++]=ch<32?' ':ch;
            }
            if(at&&yy<y0+hh-12){row[at]=0;colour(GR_RGB(248,236,202));GrText(canvas,gc,x0+6,yy,row,-1,GR_TFASCII|GR_TFTOP);}
        }
    }else if(o->type==G_BUMPER){
        ellipse(canvas,x0,y0,10,10,GR_RGB(174,72,53));ellipse(canvas,x0,y0,6,6,GR_RGB(231,168,77));
        ellipse(canvas,x0-2,y0-2,2,2,GR_RGB(252,222,163));
    }else if(o->type==G_BLACKHOLE){
        ellipse(canvas,x0,y0,8,9,GR_RGB(112,85,152));ellipse(canvas,x0,y0,5,6,GR_RGB(22,20,35));
        ellipse(canvas,(int)o->exit_x,(int)o->exit_y,6,6,GR_RGB(112,85,152));
        arrow(canvas,(int)o->exit_x,(int)o->exit_y,o->nx,o->ny,GR_RGB(211,177,237));
    }

}
static int moving_item(int index) {
    GolfLive *r=game.live+index;
    return r->object.changing||r->object.type==G_FLOATER||r->object.type==G_WINDMILL||
        (r->carrier>=0&&game.live[r->carrier].object.type==G_FLOATER);
}
static void course_background(void) {
    int i,type,level,y;static const int order[]={G_SLOPE,G_SAND,G_WATER,G_BRIDGE,G_FLOATER,G_WINDMILL,G_CUP,G_BLACKHOLE,G_BUMPER,G_SIGN,G_WALL};
    canvas=ground;
    for(y=0;y<400;y+=16)box(canvas,0,y,400,16,(y/16)&1?GR_RGB(39,100,65):GR_RGB(37,95,62));
    /* Animated courses retain ground separately. Raised objects are composited
     * after platforms, so carried cups and bridge contents stay above them. */
    for(level=0;level<2;level++)for(type=0;type<11;type++)for(i=0;i<game.hole->count;i++){
        GolfLive *r=game.live+i;
        if(r->object.type==order[type]&&r->level==level&&!moving_item(i)&&(!game.animated||(!level&&!golf_rectangle(r->object.type)&&r->object.type!=G_CUP&&r->object.type!=G_BUMPER&&r->object.type!=G_BLACKHOLE&&(r->object.type!=G_SLOPE||r->object.ground))))terrain(&r->object);
    }
    if(game.hole->borders){
        box(canvas,0,0,400,3,GR_RGB(151,127,91));box(canvas,0,397,400,3,GR_RGB(84,62,48));
        box(canvas,0,0,3,400,GR_RGB(151,127,91));box(canvas,397,0,3,400,GR_RGB(84,62,48));
    }
    lastrevision=~0UL;
}
static void scene(void){
    int i,type,level;static const int order[]={G_SLOPE,G_SAND,G_WATER,G_BRIDGE,G_FLOATER,G_WINDMILL,G_CUP,G_BLACKHOLE,G_BUMPER,G_SIGN,G_WALL};
    GolfObject edges[7];
    if(lastrevision==game.revision)return;
    GrCopyArea(background,gc,0,0,400,400,ground,0,0,MWROP_COPY);canvas=background;
    for(level=0;level<2;level++)for(type=0;type<11;type++)for(i=0;i<game.hole->count;i++){
        GolfLive *r=game.live+i;
        if(r->visible&&r->level==level&&r->object.type==order[type]&&
            (moving_item(i)||(game.animated&&(level||golf_rectangle(r->object.type)||r->object.type==G_CUP||r->object.type==G_BUMPER||r->object.type==G_BLACKHOLE||(r->object.type==G_SLOPE&&!r->object.ground)))))terrain(&r->object);
    }
    /* Moving gate geometry and visible side walls match the engine exactly. */
    for(i=0;i<game.hole->count;i++)if(golf_rectangle(game.live[i].object.type)){
        int j,n=golf_edges(game.live+i,edges);for(j=0;j<n;j++)terrain(edges+j);
    }
    lastrevision=game.revision;
}
static void button(int y,const char *key,const char *title) {
    char shortcut[6],label[14];
    box(window,SX,y,170,25,GR_RGB(43,60,65));
    line(window,SX,y,SX+169,y,GR_RGB(77,96,95));
    /* The fixed 8px font gets separate bounded shortcut/action columns. */
    snprintf(shortcut,sizeof(shortcut),"%s",key);snprintf(label,sizeof(label),"%s",title);
    text(SX+8,y+4,shortcut,GR_RGB(225,226,204));text(SX+56,y+4,label,GR_RGB(225,226,204));
}
static void sidebar(void) {
    char s[100];int i,total=game.strokes,par=0;
    box(window,418,24,192,406,GR_RGB(24,37,41));
    text(SX,32,"MINI GOLF",GR_RGB(223,184,106));
    snprintf(s,sizeof(s),"Hole %d / %d",hole+1,course->count);text(SX,62,s,GR_RGB(235,237,218));
    snprintf(s,sizeof(s),"Par %d   Shots %d",game.hole->par,game.strokes);text(SX,84,s,GR_RGB(163,189,177));
    for(i=0;i<hole;i++){total+=scores[i];par+=course->holes[i].par;}
    snprintf(s,sizeof(s),"Total %d / par %d",total,par+game.hole->par);text(SX,106,s,GR_RGB(163,189,177));
    text(SX,141,paused?"PAUSED":game.state==G_HAZARD?"WATER HAZARD":game.state==G_WARPING?"WARP IN TRANSIT":game.state==G_ROLLING?"BALL IN PLAY":game.state==G_HOLED?"IN THE CUP!":game.state==G_LIMIT?"STROKE LIMIT":putt.phase==PUTT_CHARGE?"BACKSWING":putt.phase==PUTT_PRECISION?"SET ACCURACY":putt.phase==PUTT_RETURN?"SWINGING":"READY TO PUTT",GR_RGB(223,184,106));
    if(game.state!=G_HAZARD){
        text(SX,168,putt.advanced?"A: Three-click":"A: Classic",GR_RGB(163,189,177));
        if(game.state==G_READY&&!paused){
            const char *hint=putt.phase==PUTT_RETURN?"":putt.phase==PUTT_PRECISION?"Near the white line":putt.advanced?(putt.phase==PUTT_CHARGE?"Wait for strength":"Aim, then click"):putt.phase==PUTT_CHARGE?"Release to swing":"Hold, then release";
            text(SX,190,hint,GR_RGB(216,220,202));
            button(218,"Space",putt.advanced?(putt.phase==PUTT_PRECISION?"Confirm":putt.phase==PUTT_CHARGE?"Set power":"Putt"):"Putt");
        }else if(golf_finished(&game))button(218,"Enter",hole+1==course->count?"New round":"Next hole");
        button(249,"R","Retry hole");button(280,"N","New round");button(311,"U","Undo shot");button(342,"F1","Help / About");
        text(SX,375,"G: guide  P: pause",GR_RGB(163,189,177));
    }
    /* Reserve the bottom line for outcomes and errors, not control hints. */
    snprintf(s,22,"%s",message);text(SX,400,s,GR_RGB(216,220,202));
    hud_dirty=0;
}
static void gauge_position(int *x,int *y){
    int bx=(int)game.x,by=(int)game.y;
    *x=bx<200?bx+66:bx-66;*y=by<200?by+66:by-66;
    if(*x<43)*x=43;if(*x>356)*x=356;if(*y<43)*y=43;if(*y>356)*y=356;
}
static void dynamic_bounds(int *x,int *y,int *w,int *h) {
    int xx,yy,left=(int)game.x-8,top=(int)game.y-8,right=(int)game.x+9,bottom=(int)game.y+9;
    if(game.state==G_READY&&!help&&!choosing){
        xx=(int)game.x-(long)aim_x*putt.pull/256;yy=(int)game.y-(long)aim_y*putt.pull/256;
        if(xx-13<left)left=xx-13;if(xx+14>right)right=xx+14;
        if(yy-13<top)top=yy-13;if(yy+14>bottom)bottom=yy+14;
        if(guide&&putt.phase==PUTT_IDLE){
            xx=(int)game.x+(long)aim_x*38/256;yy=(int)game.y+(long)aim_y*38/256;
            if(xx-2<left)left=xx-2;if(xx+3>right)right=xx+3;
            if(yy-2<top)top=yy-2;if(yy+3>bottom)bottom=yy+3;
        }
    }
    if(putt.advanced&&swinging()){
        gauge_position(&xx,&yy);
        if(xx-44<left)left=xx-44;if(xx+45>right)right=xx+45;
        if(yy-44<top)top=yy-44;if(yy+45>bottom)bottom=yy+45;
    }
    if(left<0)left=0;if(top<0)top=0;if(right>400)right=400;if(bottom>400)bottom=400;
    *x=left;*y=top;*w=right-left;*h=bottom-top;
}
static GR_BITMAP dial_arc[516];
static void dial_span(int row,int left,int right){
    int first=left/16,last=right/16,i;
    unsigned a=0xffffU>>(left&15),b=0xffffU<<(15-(right&15));
    GR_BITMAP *bits=dial_arc+row*6;
    if(first==last)bits[first]|=a&b;
    else{bits[first]|=a;for(i=first+1;i<last;i++)bits[i]=0xffffU;bits[last]|=b;}
}
static void putting_indicator(void){
    int cx,cy,n,xx,yy,lo,hi,start,inside,red,ex,ey;long cross;GR_COLOR arc;
    gauge_position(&cx,&cy);cx+=BX;cy+=BY;
    GrCopyArea(window,gc,cx-43,cy-43,86,86,dial_face,0,0,MWROP_COPY);
    n=(int)putt.gauge/5;if(n>72)n=72;if(n< -9)n=-9;
    red=(int)putt.gauge*255L/360;if(red<0)red=0;if(red>255)red=255;
    arc=putt.gauge<0?GR_RGB(45,45,45):GR_RGB(red,0,255-red);
    ex=-dial_points[abs(n)][0];ey=-dial_points[abs(n)][1];
    /* Compose a monochrome arc locally and submit one bitmap. Per-row drawing
     * requests make the timed three-click mode unusable on the emulated CPU. */
    memset(dial_arc,0,sizeof(dial_arc));
    for(yy=-38;yy<=38&&n;yy++){
        int half;
        for(half=0;half<2;half++){
            lo=dial_spans[yy+38][half*2];hi=dial_spans[yy+38][half*2+1];start=100;
            for(xx=lo;xx<=hi+1;xx++){
                int x=n<0?-xx:xx;
                cross=(long)x*ey-(long)yy*ex;
                inside=xx<=hi&&(abs(n)==72||(abs(n)<=36?(x<=0&&cross>=0):(x<=0||cross>=0)));
                if(inside&&start==100)start=xx;
                if(!inside&&start!=100){dial_span(yy+43,start+43,xx+42);start=100;}
            }
        }
    }
    colour(arc);GrBitmap(window,gc,cx-43,cy-43,86,86,dial_arc);
    line(window,cx,cy+43,cx,cy+27,GR_RGB(255,255,233));
    line(window,cx-3,cy,cx+3,cy,GR_RGB(176,193,166));
}
static void overlay(void) {
    int x=35,y=105;
    box(window,x+4,y+4,350,230,GR_RGB(17,31,30));box(window,x,y,350,230,GR_RGB(37,53,57));
    text(x+16,y+15,help?"KOLF / RIPLIX + FUSE-X":game.state==G_LIMIT?"STROKE LIMIT REACHED":"BALL HOLED!",GR_RGB(229,187,103));
    if(help){
        text(x+16,y+49,"Aim towards the mouse pointer",GR_RGB(226,231,212));
        text(x+16,y+73,"Hold mouse/Space/Down; release",GR_RGB(226,231,212));
        text(x+16,y+97,"Left/Right: aim (Shift/Ctrl)",GR_RGB(226,231,212));
        text(x+16,y+121,"A: three-click  Esc: cancel",GR_RGB(226,231,212));
        text(x+16,y+145,"3 clicks: begin, power, accuracy",GR_RGB(163,189,177));
        text(x+16,y+169,"U / Ctrl-Z: undo  C: courses",GR_RGB(163,189,177));
        text(x+16,y+201,"Esc / Enter / click: return",GR_RGB(229,187,103));
    }else{
        char s[100];int i,total=game.strokes;
        for(i=0;i<hole;i++)total+=scores[i];
        snprintf(s,sizeof(s),"%d strokes   /   par %d",game.strokes,game.hole->par);text(x+16,y+55,s,GR_RGB(226,231,212));
        if(hole+1==course->count){
            text(x+16,y+95,"ROUND COMPLETE",GR_RGB(229,187,103));
            snprintf(s,sizeof(s),"Total: %d strokes",total);text(x+16,y+127,s,GR_RGB(226,231,212));
        }else text(x+16,y+103,game.state==G_LIMIT?"Ball was not holed; retry with R":"Ready for the next green?",GR_RGB(163,189,177));
        text(x+16,y+171,hole+1==course->count?"Enter: new round   R: retry":"Enter: next hole   R: retry",GR_RGB(229,187,103));
    }
}
static void water_dialog(void){
    int x=35,y=105,i;
    box(window,x+4,y+4,350,230,GR_RGB(17,31,30));box(window,x,y,350,230,GR_RGB(37,53,57));
    text(x+16,y+15,"WATER HAZARD",GR_RGB(229,187,103));
    text(x+16,y+49,"Water penalty: +1 stroke",GR_RGB(226,231,212));
    for(i=0;i<2;i++){
        box(window,x+12,y+83+i*42,326,34,i==water_choice?GR_RGB(61,96,89):GR_RGB(43,60,65));
        text(x+22,y+91+i*42,i?"R  Rehit from last location":"D  Drop outside hazard",GR_RGB(226,231,212));
    }
    text(x+16,y+170,"Arrows / Tab: choose",GR_RGB(163,189,177));
    text(x+16,y+201,"Enter: confirm   U: undo",GR_RGB(229,187,103));
}
static void course_picker(void){
    int i;box(window,35,72,350,308,GR_RGB(37,53,57));
    text(51,86,"CHOOSE A COURSE",GR_RGB(229,187,103));
    for(i=0;i<BUILTIN_COUNT;i++){
        if(i==choice)box(window,45,116+i*26,330,25,GR_RGB(61,96,89));
        text(57,120+i*26,builtins[i].name,GR_RGB(226,231,212));
    }
    text(51,345,"Up/Down  Enter: play  Esc: cancel",GR_RGB(229,187,103));
}
static void select_course(int index);
static void paint(void) {
    int x,y,w,h,i;char title[70];
    scene();dynamic_bounds(&x,&y,&w,&h);
    if(dirty){
        box(window,0,0,WIDTH,HEIGHT,GR_RGB(18,30,33));
        snprintf(title,sizeof(title),"KOLF  /  %.45s",course->name);text(10,3,title,GR_RGB(223,220,187));text(SX,3,"C: Courses",GR_RGB(229,187,103));
        GrCopyArea(window,gc,BX,BY,400,400,background,0,0,MWROP_COPY);hud_dirty=1;
    }else if(game.animated){
        GrCopyArea(window,gc,BX,BY,400,400,background,0,0,MWROP_COPY);
    }else{
        if(oldw>0&&oldh>0)GrCopyArea(window,gc,BX+oldx,BY+oldy,oldw,oldh,background,oldx,oldy,MWROP_COPY);
        if(w>0&&h>0)GrCopyArea(window,gc,BX+x,BY+y,w,h,background,x,y,MWROP_COPY);
    }
    GrSetGCRegion(gc,board_region);
    if(game.state==G_READY&&!help&&!choosing){
        int px=BX+(int)game.x-(long)aim_x*putt.pull/256;
        int py=BY+(int)game.y-(long)aim_y*putt.pull/256;
        if(putt.phase==PUTT_IDLE&&guide)for(i=10;i<36;i+=5)line(window,BX+(int)game.x+(long)aim_x*i/256,BY+(int)game.y+(long)aim_y*i/256,
            BX+(int)game.x+(long)aim_x*(i+3)/256,BY+(int)game.y+(long)aim_y*(i+3)/256,GR_RGB(203,208,156));
        line(window,px+(long)aim_y*11/256,py-(long)aim_x*11/256,px-(long)aim_y*11/256,py+(long)aim_x*11/256,GR_RGB(10,21,19));
        line(window,px+(long)aim_y*11/256+1,py-(long)aim_x*11/256,px-(long)aim_y*11/256+1,py+(long)aim_x*11/256,GR_RGB(224,207,162));
        if(putt.advanced&&swinging())putting_indicator();
    }
    if(game.state!=G_WARPING&&game.state!=G_LIMIT&&game.state!=G_HAZARD){
    ellipse(window,BX+(int)game.x+2,BY+(int)game.y+3,5,3,GR_RGB(22,55,39));
    ellipse(window,BX+(int)game.x,BY+(int)game.y,4,4,GR_RGB(237,235,211));
    box(window,BX+(int)game.x-1,BY+(int)game.y-2,2,2,GR_RGB(255,255,241));
    }
    GrSetGCRegion(gc,0);
    if(hud_dirty)sidebar();
    if(help||golf_finished(&game))overlay();
    if(choosing)course_picker();
    if(game.state==G_HAZARD)water_dialog();
    GrFlush();dirty=0;oldx=x;oldy=y;oldw=w;oldh=h;frames++;
    if(trace&&!swinging()&&(paused||!golf_active(&game))){
        GR_WINDOW_INFO info;
        /* The server consumes preceding drawing before replying. Tests must
         * distinguish an acknowledged input from a completed paused frame. */
        GrGetWindowInfo(window,&info);
        fprintf(stderr,"KOLF_PAINTED %d %d %d\n",hole+1,paused,game.state);
    }
}
static void begin_hole(int index) {
    GR_WM_PROPERTIES props;char title[100];
    hole=index;golf_begin(&game,course->holes+hole);paused=help=choosing=water_choice=0;cancel_swing();angle=undo_angle=PI;
    aim_x=(int)(cos(angle)*256);aim_y=(int)(sin(angle)*256);
    memset(&props,0,sizeof(props));snprintf(title,sizeof(title),"Kolf / %.75s",course->name);props.flags=GR_WM_FLAGS_TITLE;props.title=title;GrSetWMProperties(window,&props);
    message[0]=0;laststate=laststrokes=-1;course_background();dirty=hud_dirty=1;next_tick=milliseconds()+11;
    if(trace)fprintf(stderr,"KOLF_HOLE %d %d %d\n",hole+1,game.hole->par,game.hole->count);
}
static void advance_hole(void){
    if(hole+1<course->count){scores[hole]=game.strokes;begin_hole(hole+1);}
    else{memset(scores,0,sizeof(scores));begin_hole(0);}
}
static unsigned long putting_time(unsigned long now){
    /* Like Qt's coalesced timer, a slow paint must not consume an entire
     * input window. Keep ordinary 20/2ms ticks, but bound display-stall catch-up
     * just as we bound the physics catch-up below. */
    if(swinging()&&(long)(now-putt.next)>80)putt.next=now-80;
    return now;
}
static void swing_update(unsigned long now){
    int phase=putt.phase;
    if(game.state!=G_READY){if(putt.phase!=PUTT_IDLE)cancel_swing();return;}
    putt_update(&putt,putting_time(now));
    if(phase!=putt.phase){hud_dirty=1;if(trace)fprintf(stderr,"KOLF_SWING %d %d %d\n",putt.advanced,putt.phase,(int)(putt.strength*100));}
    if(putt.phase==PUTT_FIRE){
        double power=putt_power(&putt);angle-=putt.correction*PI/180;
        aim_x=(int)(cos(angle)*256);aim_y=(int)(sin(angle)*256);
        if(golf_shoot(&game,cos(angle),sin(angle),power)){
            undo_angle=angle;
            next_tick=now+11;message[0]=0;hud_dirty=1;
            if(trace)fprintf(stderr,"KOLF_SHOT %d %d power=%ld correction=%d aim=%d,%d\n",hole+1,game.strokes,(long)(power*1000),putt.correction,aim_x,aim_y);
        }
        putt_cancel(&putt);
    }
}
static void press_stroke(int input){
    int phase;
    if(help||choosing||paused)return;
    if(golf_finished(&game)){advance_hole();return;}
    if(game.state!=G_READY)return;
    swing_update(milliseconds());if(game.state!=G_READY)return;
    phase=putt.phase;
    if(!putt.advanced&&phase!=PUTT_IDLE)return;
    putt_press(&putt,putting_time(milliseconds()));if(phase==PUTT_IDLE)owner=input;
    hud_dirty=1;
    if(trace)fprintf(stderr,"KOLF_SWING %d %d %d\n",putt.advanced,putt.phase,(int)(putt.strength*100));
}
static void release_stroke(int input){
    if(owner==input&&!help&&!choosing&&!paused){putt_release(&putt,putting_time(milliseconds()));hud_dirty=1;}
}
static void undo_shot(void){
    cancel_swing();
    if(!golf_undo(&game)){strcpy(message,"No shot to undo");return;}
    angle=undo_angle;aim_x=(int)(cos(angle)*256);aim_y=(int)(sin(angle)*256);
    help=choosing=water_choice=0;next_tick=milliseconds()+11;dirty=hud_dirty=1;
    strcpy(message,"Shot undone");
    if(trace)fprintf(stderr,"KOLF_UNDO %d %d %ld %ld\n",hole+1,game.strokes,(long)(game.x*1000),(long)(game.y*1000));
}
static void recover_ball(int choice){
    if(!golf_recover(&game,choice)){
        water_choice=1;strcpy(message,"Drop blocked; rehit");dirty=hud_dirty=1;return;
    }
    cancel_swing();dirty=hud_dirty=1;next_tick=milliseconds()+11;
    strcpy(message,choice==G_DROP?"Dropped; penalty +1":"Rehit; penalty +1");
    if(trace)fprintf(stderr,"KOLF_RECOVER %d %d %ld %ld\n",choice,game.strokes,(long)(game.x*1000),(long)(game.y*1000));
}
static void keyboard(unsigned key,unsigned modifiers,int down) {
    int *held=key==' '?&space_held:key==MWKEY_DOWN?&down_held:NULL;
    if(!down){if(held){*held=0;release_stroke(key==' '?2:3);}return;}
    if(held&&*held)return;
    if(held)*held=1;
    if(choosing){
        if(key==MWKEY_ESCAPE){choosing=0;dirty=1;next_tick=milliseconds()+11;}
        else if(key==MWKEY_UP){choice=(choice+BUILTIN_COUNT-1)%BUILTIN_COUNT;dirty=1;}
        else if(key==MWKEY_DOWN){choice=(choice+1)%BUILTIN_COUNT;dirty=1;}
        else if(key=='\r'||key=='\n')select_course(choice);
        return;
    }
    if(help){help=0;dirty=1;next_tick=milliseconds()+11;return;}
    if(key=='q'||key=='Q'){stopping=1;return;}
    if(key=='u'||key=='U'||key==26||((modifiers&MWKMOD_CTRL)&&(key=='z'||key=='Z'))){undo_shot();return;}
    if(game.state==G_HAZARD){
        if(key=='d'||key=='D')recover_ball(G_DROP);
        else if(key=='r'||key=='R')recover_ball(G_REHIT);
        else if(key=='\r'||key=='\n')recover_ball(water_choice);
        else if(key==MWKEY_UP||key==MWKEY_DOWN||key==MWKEY_LEFT||key==MWKEY_RIGHT||key=='\t'){water_choice=!water_choice;dirty=1;}
        return;
    }
    if(key=='c'||key=='C'){cancel_swing();choosing=1;choice=selected_course;dirty=1;return;}
    if(key==MWKEY_ESCAPE){cancel_swing();return;}
    if(key==MWKEY_F1||key=='?'){cancel_swing();help=1;dirty=1;return;}
    if(key=='n'||key=='N'){memset(scores,0,sizeof(scores));begin_hole(0);return;}
    if(key=='r'||key=='R'){begin_hole(hole);return;}
    if(key=='a'||key=='A'){cancel_swing();putt.advanced=!putt.advanced;message[0]=0;hud_dirty=1;return;}
    if(key=='g'||key=='G'){guide=!guide;hud_dirty=1;return;}
    if(key=='p'||key=='P'){cancel_swing();paused=!paused;hud_dirty=1;next_tick=milliseconds()+11;if(trace)fprintf(stderr,"KOLF_PAUSE %d\n",paused);return;}
    if(paused)return;
    if(held){press_stroke(key==' '?2:3);return;}
    if(key=='\r'||key=='\n'){if(golf_finished(&game))advance_hole();return;}
    if(game.state!=G_READY||(putt.advanced&&swinging()))return;
    if(key==MWKEY_LEFT||key==MWKEY_RIGHT){
        double step=modifiers&MWKMOD_SHIFT?6:modifiers&MWKMOD_CTRL?.5:2;
        angle+=(key==MWKEY_LEFT?-step:step)*PI/180;
        aim_x=(int)(cos(angle)*256);aim_y=(int)(sin(angle)*256);hud_dirty=1;
    }
}
static void pointer(int x,int y) {
    double dx=(x-BX)-game.x,dy=(y-BY)-game.y;
    if(help||choosing||paused||game.state!=G_READY||(putt.advanced&&swinging())||x<BX||x>=BX+400||y<BY||y>=BY+400)return;
    if(dx*dx+dy*dy<4)return;
    angle=atan2(dy,dx);aim_x=(int)(cos(angle)*256);aim_y=(int)(sin(angle)*256);hud_dirty=1;
}
static void signal_stop(int sig) {
    int saved=errno;char c=1;signal(sig,signal_stop);stopping=1;
    if(wake_pipe[1]>=0)write(wake_pipe[1],&c,1);errno=saved;
}
static void cleanup(void) {
    int writer=wake_pipe[1];wake_pipe[1]=-1;
    if(wake_pipe[0]>=0){if(connected)GrUnregisterInput(wake_pipe[0]);close(wake_pipe[0]);wake_pipe[0]=-1;}
    if(writer>=0)close(writer);
    if(connected){connected=0;GrClose();}golf_free(course);course=NULL;
    if(trace)fprintf(stderr,"KOLF_EXIT frames=%lu steps=%lu\n",frames,steps);
}
static GolfCourse *builtin_course(int index,char *error,unsigned size){
    const BuiltinCourse *b=builtins+index;char *data=malloc(b->length);uint32_t length=b->length;GolfCourse *c;
    if(!data){snprintf(error,size,"Out of memory");return NULL;}
    if(ri_deflate(data,&length,b->text,b->packed)!=TINF_OK||length!=b->length){
        free(data);snprintf(error,size,"Invalid built-in course data");return NULL;
    }
    c=golf_course(data,length,error,size);free(data);return c;
}
static GolfCourse *read_course(const char *path,char *error,unsigned size) {
    int fd,n;struct stat st;char *data;unsigned long at=0;GolfCourse *c;
    if(!path)return builtin_course(selected_course,error,size);
    for(n=0;n<BUILTIN_COUNT;n++)if(!strcmp(path,builtins[n].name)){
        selected_course=n;return builtin_course(n,error,size);
    }
    fd=open(path,O_RDONLY|O_NONBLOCK);
    if(fd<0){snprintf(error,size,"Cannot open course: %s",path);return NULL;}
    if(fstat(fd,&st)<0||!S_ISREG(st.st_mode)||st.st_size<1||st.st_size>262144L){close(fd);snprintf(error,size,"Course must be a regular file <=256 KiB");return NULL;}
    data=malloc((unsigned long)st.st_size);if(!data){close(fd);snprintf(error,size,"Out of memory");return NULL;}
    while(at<(unsigned long)st.st_size){
        unsigned amount=(unsigned long)st.st_size-at>4096?4096:(unsigned)((unsigned long)st.st_size-at);
        n=read(fd,data+at,amount);if(n<0&&errno==EINTR)continue;
        if(n<=0){free(data);close(fd);snprintf(error,size,"Cannot read complete course");return NULL;}at+=n;
    }
    close(fd);c=golf_course(data,at,error,size);free(data);return c;
}
static void select_course(int index){
    GolfCourse *next;char error[160];int previous=selected_course;
    selected_course=index;next=read_course(NULL,error,sizeof(error));
    if(!next){selected_course=previous;snprintf(message,sizeof(message),"%s",error);choosing=0;dirty=hud_dirty=1;return;}
    golf_free(course);course=next;memset(scores,0,sizeof(scores));begin_hole(0);
}
static void usage(void) {
    puts("usage: kolf [--advanced] [COURSE]\n       kolf --check-course [COURSE]\n       kolf --benchmark [STEPS-PER-HOLE]\n"
         "Eight built-in KDE courses: --list-courses; C opens the course selector.\n"
         "Aim towards the mouse pointer; hold/release mouse, Space or Down to putt.\n"
         "A: three-click putting. Escape cancels. Left/Right aim; Shift/Ctrl adjust.\nU / Ctrl-Z: undo shot. Water: D drops, R rehits, Enter confirms choice.\nEnter: next hole. R retries, N restarts, P pauses, G guide, F1 helps.\n"
         "Original object types, moving obstacles, bridge layers and warps supported.\n"
         "KDE Kolf-derived rules/course; GPL-2.0-or-later. New C collision engine.");
}
int main(int argc,char **argv) {
    char error[160];const char *path=NULL;int check=0,benchmark=0,i,j,again;
    long bench_steps=1000;char *end;
    GR_EVENT event;GR_RECT rect;GR_WINDOW_INFO ready_info;unsigned long now,start,elapsed;
    for(i=1;i<argc;i++){
        if(!strcmp(argv[i],"--list-courses")){for(j=0;j<BUILTIN_COUNT;j++)puts(builtins[j].name);return 0;}
        if(!strcmp(argv[i],"--help")){usage();return 0;}
        else if(!strcmp(argv[i],"--advanced"))putt.advanced=1;
        else if(!strcmp(argv[i],"--check-course"))check=1;
        else if(!strcmp(argv[i],"--benchmark")){
            benchmark=1;
            if(i+1<argc&&argv[i+1][0]!='-'){
                bench_steps=strtol(argv[++i],&end,10);
                if(*end||bench_steps<1||bench_steps>30000){usage();return 1;}
            }
        }
        else if(argv[i][0]=='-'||path){usage();return 1;}else path=argv[i];
    }
    putt_init(&putt,putt.advanced);putt.random=milliseconds()|1UL;
    trace=getenv("KOLF_TRACE")!=NULL;course=read_course(path,error,sizeof(error));
    if(!course){fprintf(stderr,"kolf: %s\n",error);return 1;}
    if(check){printf("KOLF_COURSE_OK holes=%d name=%s\n",course->count,course->name);golf_free(course);return 0;}
    if(benchmark){
        start=milliseconds();
        for(i=0;i<course->count;i++){
            golf_begin(&game,course->holes+i);golf_shoot(&game,-1,-1,5);
            for(j=0;j<bench_steps;j++){if(!golf_active(&game)||golf_finished(&game)){golf_begin(&game,course->holes+i);golf_shoot(&game,1,-1,5);}golf_step(&game);}
        }
        elapsed=milliseconds()-start;printf("KOLF_BENCH steps=%ld ms=%lu x=%ld y=%ld\n",(long)course->count*bench_steps,elapsed,(long)(game.x*1000),(long)(game.y*1000));golf_free(course);return 0;
    }
    if(GrOpen()<0){fprintf(stderr,"kolf: start the Nano-X desktop first (startx)\n");golf_free(course);return 1;}
    connected=1;atexit(cleanup);
    window=GrNewWindowEx(GR_WM_PROPS_APPWINDOW|GR_WM_PROPS_NORESIZE,"Kolf / Practice",GR_ROOT_WINDOW_ID,8,4,WIDTH,HEIGHT,GR_RGB(18,30,33));
    gc=GrNewGC();background=GrNewPixmapEx(400,400,MWIF_RGB565,NULL);ground=GrNewPixmapEx(400,400,MWIF_RGB565,NULL);dial_face=GrNewPixmapEx(86,86,MWIF_RGB565,NULL);board_region=GrNewRegion();
    if(!window||!gc||!background||!ground||!dial_face||!board_region){fprintf(stderr,"kolf: not enough graphics resources\n");return 1;}
    GrSetGCUseBackground(gc,GR_FALSE);
    box(dial_face,0,0,86,86,GR_RGB(23,40,39));
    colour(GR_RGB(229,231,216));GrBitmap(dial_face,gc,0,0,86,86,(GR_BITMAP *)dial_mask);
    rect.x=BX;rect.y=BY;rect.width=400;rect.height=400;
    GrUnionRectWithRegion(board_region,&rect);
    GrSelectEvents(window,GR_EVENT_MASK_EXPOSURE|GR_EVENT_MASK_CLOSE_REQ|GR_EVENT_MASK_KEY_DOWN|GR_EVENT_MASK_KEY_UP|
        GR_EVENT_MASK_BUTTON_DOWN|GR_EVENT_MASK_BUTTON_UP|GR_EVENT_MASK_MOUSE_POSITION|GR_EVENT_MASK_FOCUS_OUT);
    if(pipe(wake_pipe)<0)return 1;
    fcntl(wake_pipe[0],F_SETFL,O_NONBLOCK);fcntl(wake_pipe[1],F_SETFL,O_NONBLOCK);GrRegisterInput(wake_pipe[0]);
    signal(SIGINT,signal_stop);signal(SIGTERM,signal_stop);signal(SIGHUP,signal_stop);
    GrMapWindow(window);GrSetFocus(window);begin_hole(0);paint();next_frame=milliseconds()+33;
    /* Flush submits asynchronously. The query also establishes that map/focus
     * and the first committed repaint have been consumed before READY. */
    GrGetWindowInfo(window,&ready_info);
    if(trace)fprintf(stderr,"KOLF_READY %d\n",getpid());
    while(!stopping){
        now=milliseconds();if(!paused&&!help&&!choosing)swing_update(now);again=golf_active(&game)&&!paused&&!help&&!choosing;
        /* At most one display frame per 33ms, even under pointer floods. */
        if(again){
            for(i=0;i<8&&(long)(now-next_tick)>=0&&golf_active(&game);i++){
                golf_step(&game);steps++;next_tick+=11;
                if(game.event==G_SUNK||game.event==G_WARP||game.event==G_SPLASH||game.state==G_LIMIT)message[0]=0;
                if(game.event==G_OUT)strcpy(message,"Out: rehit (1 shot)");
                if(game.event==G_SPLASH){cancel_swing();water_choice=0;dirty=hud_dirty=1;}
                if(game.event==G_OUT){next_tick=now+11;break;}
            }
            if(i==8&&(long)(now-next_tick)>=0)next_tick=now+11; /* Bound catch-up after a stall. */
        }
        if(game.state!=laststate||game.strokes!=laststrokes){hud_dirty=1;laststate=game.state;laststrokes=game.strokes;
            if(trace)fprintf(stderr,"KOLF_STATE %d %d %d %ld %ld\n",hole+1,game.state,game.strokes,(long)(game.x*1000),(long)(game.y*1000));}
        if(dirty||hud_dirty||again||swinging()){
            if(dirty||(long)(now-next_frame)>=0){paint();next_frame=milliseconds()+33;}
        }
        again=(golf_active(&game)&&!paused&&!help&&!choosing)||hud_dirty||dirty||swinging();
        GrGetNextEventTimeout(&event,again?11:GR_TIMEOUT_BLOCK);
        if(event.type==GR_EVENT_TYPE_CLOSE_REQ)stopping=1;
        else if(event.type==GR_EVENT_TYPE_EXPOSURE)dirty=1;
        else if(event.type==GR_EVENT_TYPE_FDINPUT){char bytes[16];read(wake_pipe[0],bytes,sizeof(bytes));}
        else if(event.type==GR_EVENT_TYPE_FOCUS_OUT){cancel_swing();mouse_held=space_held=down_held=0;if(golf_active(&game)){paused=1;hud_dirty=1;}}
        else if(event.type==GR_EVENT_TYPE_KEY_DOWN||event.type==GR_EVENT_TYPE_KEY_UP){keyboard(event.keystroke.ch,event.keystroke.modifiers,event.type==GR_EVENT_TYPE_KEY_DOWN);hud_dirty=1;}
        else if(event.type==GR_EVENT_TYPE_MOUSE_POSITION){pointer(event.mouse.x,event.mouse.y);hud_dirty=1;}
        else if(event.type==GR_EVENT_TYPE_BUTTON_DOWN&&(event.button.changebuttons&GR_BUTTON_L)){
            if(game.state==G_HAZARD){
                if(event.button.x>=47&&event.button.x<373){
                    if(event.button.y>=188&&event.button.y<222)recover_ball(G_DROP);
                    else if(event.button.y>=230&&event.button.y<264)recover_ball(G_REHIT);
                }
            }else if(choosing){
                i=(event.button.y-116)/26;
                if(event.button.x>=45&&event.button.x<375&&event.button.y>=116&&i<BUILTIN_COUNT)select_course(i);
            }else if(help){help=0;dirty=1;next_tick=milliseconds()+11;}
            else if(event.button.x>=SX){
                i=event.button.y;if(i>=218&&i<243){if(!mouse_held){mouse_held=1;press_stroke(1);}}else if(i>=249&&i<274)begin_hole(hole);
                else if(i>=280&&i<305){memset(scores,0,sizeof(scores));begin_hole(0);}
                else if(i>=311&&i<336)undo_shot();
                else if(i>=342&&i<367){cancel_swing();help=1;dirty=1;}
            }else if(game.state==G_READY&&!paused&&event.button.x>=BX&&event.button.x<BX+400&&event.button.y>=BY&&event.button.y<BY+400){pointer(event.button.x,event.button.y);if(!mouse_held){mouse_held=1;press_stroke(1);}}
        }else if(event.type==GR_EVENT_TYPE_BUTTON_UP&&(event.button.changebuttons&GR_BUTTON_L)){mouse_held=0;release_stroke(1);}
    }
    return 0;
}
