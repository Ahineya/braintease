/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Original Shikaku game. Shared Nano-X frontend for Riplix and Fuse-X. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <time.h>
#include <stdint.h>
#include "nano-X.h"
#include "shikaku.h"
#define BG GR_RGB(26,33,39)
#define PANEL GR_RGB(35,44,53)
#define PAPER GR_RGB(30,40,48)
#define FG GR_RGB(226,233,235)
#define MUTED GR_RGB(151,170,180)
#define GOLD GR_RGB(235,193,110)
#define GREEN GR_RGB(153,223,118)
#define RED GR_RGB(238,133,128)
#define GRID GR_RGB(48,60,69)
#define MAJOR GR_RGB(72,89,100)
static SGame game;
static GR_WINDOW_ID window,board,cache,target;
static GR_GC_ID gc;
static int width=600,height=480,tile=40,zoom,panx,pany,vx,vy,vw,vh,extent;
static int full=1,board_dirty=1,cache_dirty=1,mode,choice,dragging,erasing,ax,ay,ex,ey,cx,cy;
static int board_mapped,last_x=-1,last_y=-1,last_w=-1,last_h=-1;
static int keyboard_cursor,scroll_drag,connected,trace,save_blocked,unsaved,exit_code;
static SGenerator *generator;
static GR_TIMER_ID generation_timer;
static int generating_size,restoring;
static unsigned long generating_seed,remembered_seed[6];
static uint32_t seed_counter;
static int remembered[6],wakefd[2]={-1,-1};
static volatile int stopping;
static char directory[384],path[448],message[112];
static unsigned long frames;
static const char *buttons[]={"Size","New","Undo","Redo","Clear","Help","-","+","Fit"};
static void box(int x,int y,int w,int h,GR_COLOR colour) {
    if(w>0&&h>0){GrSetGCForeground(gc,colour);GrFillRect(target,gc,x,y,w,h);}
}
static void edge(int x,int y,int w,int h,GR_COLOR colour) {
    if(w>0&&h>0){GrSetGCForeground(gc,colour);GrRect(target,gc,x,y,w,h);}
}
static void text(int x,int y,const char *s,GR_COLOR colour) {
    GrSetGCForeground(gc,colour);GrText(target,gc,x,y,s,strlen(s),GR_TFASCII|GR_TFTOP);
}
static void say(const char *s){snprintf(message,sizeof(message),"%s",s);full=1;}
static void caught(int sig) {
    int saved=errno;char c=1;signal(sig,caught);stopping=1;
    if(wakefd[1]>=0)write(wakefd[1],&c,1);errno=saved;
}
static void remember(void) {
    char name[448],temp[480];FILE *f;int i,bad=0;
    snprintf(name,sizeof(name),"%s/current",directory);snprintf(temp,sizeof(temp),"%s.tmp-%d",name,(int)getpid());
    f=fopen(temp,"w");if(!f){perror("shikaku: remember");return;}
    if(fprintf(f,"%d\n",game.puzzle)<0)bad=1;
    for(i=0;i<6;i++)if(fprintf(f,"%d\n",remembered[i])<0)bad=1;
    for(i=0;i<6;i++)if(fprintf(f,"%lu\n",remembered_seed[i])<0)bad=1;
    if(fflush(f))bad=1;if(fclose(f))bad=1;
    if(!bad&&rename(temp,name)==0)return;unlink(temp);perror("shikaku: remember");
}
static int save(void) {
    if(!unsaved||save_blocked)return !save_blocked;
    if(s_save(&game,path)<0){perror("shikaku: save");say("Save failed. Your board is still here; S retries saving.");exit_code=1;return 0;}
    unsaved=0;
    if(trace)fprintf(stderr,"SHIKAKU_SAVE %d %d %d\n",game.puzzle,s_state(&game)->count,s_won(&game));
    return 1;
}
static void opened(void) {
    GR_WM_PROPERTIES props;char title[96];int result;
    if(game.generated)snprintf(path,sizeof(path),"%s/%dx%d-seed-%lu",directory,game.size,game.size,game.seed);
    else snprintf(path,sizeof(path),"%s/%dx%d-%02d",directory,game.size,game.size,game.puzzle%S_PER_SIZE+1);
    save_blocked=unsaved=0;result=s_load(&game,path);
    if(result<0){save_blocked=1;say("Save unreadable. Clear starts fresh; the file is kept until then.");}
    else say(result?"Saved board restored.":"One clue per rectangle; its number is the rectangle's area.");
    if(!restoring){
        remembered[game.puzzle/S_PER_SIZE]=game.puzzle%S_PER_SIZE;
        remembered_seed[game.puzzle/S_PER_SIZE]=game.generated?game.seed:0;
        remember();
    }
    panx=pany=0;zoom=0;mode=dragging=keyboard_cursor=0;cx=cy=0;
    if(connected){
        memset(&props,0,sizeof(props));
        if(game.generated)snprintf(title,sizeof(title),"Shikaku / %dx%d / seed %lu",game.size,game.size,game.seed);
        else snprintf(title,sizeof(title),"Shikaku / %dx%d / %d",game.size,game.size,game.puzzle%S_PER_SIZE+1);
        props.flags=GR_WM_FLAGS_TITLE;props.title=title;GrSetWMProperties(window,&props);
        if(cache){GrDestroyWindow(cache);cache=0;}
    }
    full=cache_dirty=board_dirty=1;
    if(trace)fprintf(stderr,"SHIKAKU_PUZZLE %d %d %d\n",game.puzzle,game.size,s_state(&game)->count);
    if(trace&&game.generated)fprintf(stderr,"SHIKAKU_SEED %d %lu %lu\n",game.size,game.seed,s_game_signature(&game));
}
static int choose(int p) {
    if(connected&&unsaved&&!save())return 0;
    if(!s_begin(&game,p))return 0;opened();return 1;
}
static void cancel_generation(void) {
    if(generation_timer)GrDestroyTimer(generation_timer);generation_timer=0;
    s_generate_free(generator);generator=NULL;mode=0;full=cache_dirty=board_dirty=1;
}
static unsigned long fresh_seed(void) {
    uint32_t seed=(uint32_t)time(NULL)^((uint32_t)getpid()<<16)^++seed_counter*2654435761UL;
    if(!seed)seed=1;return seed;
}
static void generate(int size,unsigned long seed) {
    if(unsaved&&!save())return;
    generator=s_generate_start(size,seed);
    if(!generator){say("Not enough memory to generate a board; current board kept.");mode=0;return;}
    generation_timer=GrCreateTimer(window,10);
    if(!generation_timer){s_generate_free(generator);generator=NULL;say("Cannot start the puzzle generator timer.");mode=0;return;}
    generating_size=size;generating_seed=seed;mode=4;dragging=0;full=1;
    if(trace)fprintf(stderr,"SHIKAKU_GENERATING %d %lu\n",size,seed);
}
static void generation_step(void) {
    int result,refinements=s_generate_refinements(generator);
    /* Rearm after work, so a slow guest never queues periodic timer events. */
    GrDestroyTimer(generation_timer);generation_timer=0;GrFlush();
    result=s_generate_step(generator,64);
    if(result){
        if(result>0){s_generate_finish(generator,&game);cancel_generation();opened();}
        else {cancel_generation();say("Puzzle generation failed; current board kept.");}
    }else {
        generation_timer=GrCreateTimer(window,10);
        if(!generation_timer){cancel_generation();say("Cannot continue the puzzle generator timer.");}
        else if(refinements!=s_generate_refinements(generator))full=1;
    }
}
static void select_size(int size) {
    unsigned long seed=remembered_seed[size];
    if(seed)generate(s_sizes[size],seed);else choose(size*S_PER_SIZE+remembered[size]);
}
static void clamp_pan(void) {
    int xmax=extent-vw,ymax=extent-vh;if(xmax<0)xmax=0;if(ymax<0)ymax=0;
    if(panx<0)panx=0;if(pany<0)pany=0;if(panx>xmax)panx=xmax;if(pany>ymax)pany=ymax;
}
static int layout(void) {
    int available_w=width-44,available_h=height-144,s=zoom,old=tile;GR_WINDOW_ID next;
    if(!s){s=available_w/game.size;if(available_h/game.size<s)s=available_h/game.size;if(s<10)s=10;if(s>64)s=64;}
    if(!cache||s!=tile){
        next=GrNewPixmapEx(game.size*s+1,game.size*s+1,MWIF_RGB565,NULL);
        if(!next){say("Not enough graphics memory for this zoom.");zoom=old;if(!cache)return 0;s=old;}
        else {if(cache)GrDestroyWindow(cache);cache=next;tile=s;cache_dirty=1;}
    }
    extent=game.size*tile+1;vw=extent<available_w?extent:available_w;vh=extent<available_h?extent:available_h;
    vx=(width-vw)/2;vy=88+(available_h-vh)/2;
    if(vx!=last_x||vy!=last_y){GrMoveWindow(board,vx,vy);last_x=vx;last_y=vy;}
    if(vw!=last_w||vh!=last_h){GrResizeWindow(board,vw,vh);last_w=vw;last_h=vh;}
    clamp_pan();return 1;
}
static void number(int x,int y,int value,GR_COLOR colour) {
    char s[12];int digits,i,row,col,bits,start;
    static const unsigned char small[10][5]={{7,5,5,5,7},{2,6,2,2,7},{7,1,7,4,7},{7,1,7,1,7},{5,5,7,1,1},{7,4,7,1,7},{7,4,7,5,7},{7,1,1,1,1},{7,5,7,5,7},{7,5,7,1,7}};
    snprintf(s,sizeof(s),"%d",value);digits=strlen(s);
    if(tile>=20){text(x+(tile-digits*8)/2,y+(tile-16)/2,s,colour);return;}
    x+=(tile-(digits*4-1))/2;y+=(tile-5)/2;
    for(i=0;i<digits;i++)for(row=0;row<5;row++){
        bits=small[s[i]-'0'][row];col=0;
        while(col<3){if(!(bits&(4>>col))){col++;continue;}start=col;while(col<3&&(bits&(4>>col)))col++;box(x+i*4+start,y+row,col-start,1,colour);}
    }
}
static void grid(SRect r,int ox,int oy) {
    int x,y;char dashes[2]={2,3};
    GrSetGCDash(gc,dashes,2);
    for(x=r.x;x<=r.x+r.w;x++){
        GrSetGCForeground(gc,x%5==0||x==game.size?MAJOR:GRID);
        GrSetGCLineAttributes(gc,x%5==0||x==game.size?GR_LINE_SOLID:GR_LINE_ONOFF_DASH);
        GrLine(target,gc,x*tile+ox,r.y*tile+oy,x*tile+ox,(r.y+r.h)*tile+oy);
    }
    for(y=r.y;y<=r.y+r.h;y++){
        GrSetGCForeground(gc,y%5==0||y==game.size?MAJOR:GRID);
        GrSetGCLineAttributes(gc,y%5==0||y==game.size?GR_LINE_SOLID:GR_LINE_ONOFF_DASH);
        GrLine(target,gc,r.x*tile+ox,y*tile+oy,(r.x+r.w)*tile+ox,y*tile+oy);
    }
    GrSetGCLineAttributes(gc,GR_LINE_SOLID);
}
static void clues(SRect r,int ox,int oy) {
    int x,y,n;for(y=r.y;y<r.y+r.h;y++)for(x=r.x;x<r.x+r.w;x++)if((n=game.clues[y*game.size+x]))number(x*tile+ox,y*tile+oy,n,FG);
}
static SRect selection(void) {
    SRect r;r.x=ax<ex?ax:ex;r.y=ay<ey?ay:ey;r.w=abs(ex-ax)+1;r.h=abs(ey-ay)+1;return r;
}
static GR_COLOR fill_colour(SRect r) {
    unsigned colour=(r.x*3+r.y*5+r.w+r.h)%4;
    if(s_status(&game,r)!=1)return GR_RGB(76,44,48);
    return colour==0?GR_RGB(44,65,74):colour==1?GR_RGB(46,69,65):colour==2?GR_RGB(62,59,76):GR_RGB(70,63,48);
}
static void build_cache(void) {
    int i;SState *s=s_state(&game);SRect all={0,0,game.size,game.size};target=cache;box(0,0,extent,extent,PAPER);
    for(i=0;i<s->count;i++){SRect r=s->rects[i];box(r.x*tile,r.y*tile,r.w*tile,r.h*tile,fill_colour(r));}
    grid(all,0,0);
    for(i=0;i<s->count;i++){
        SRect r=s->rects[i];GR_COLOR c=s_status(&game,r)==1?GOLD:RED;
        edge(r.x*tile+1,r.y*tile+1,r.w*tile-1,r.h*tile-1,c);
        if(tile>=14)edge(r.x*tile+2,r.y*tile+2,r.w*tile-3,r.h*tile-3,c);
    }
    clues(all,0,0);cache_dirty=0;
}
static void paint_board(void) {
    SRect r;
    if(cache_dirty)build_cache();target=board;
    GrCopyArea(board,gc,0,0,vw,vh,cache,panx,pany,MWROP_COPY);
    if(dragging){
        r=selection();box(r.x*tile-panx+1,r.y*tile-pany+1,r.w*tile-1,r.h*tile-1,
            erasing?GR_RGB(96,45,52):s_status(&game,r)==1?GR_RGB(61,91,77):GR_RGB(52,76,97));
        grid(r,-panx,-pany);clues(r,-panx,-pany);
        edge(r.x*tile-panx+1,r.y*tile-pany+1,r.w*tile-1,r.h*tile-1,erasing?RED:s_status(&game,r)==1?GREEN:GOLD);
    }else if(keyboard_cursor)edge(cx*tile-panx+3,cy*tile-pany+3,tile-5,tile-5,FG);
    board_dirty=0;
}
static void modal(void) {
    int i,x,y;char label[40];target=window;box(0,0,width,height,BG);
    if(mode==1){
        text(20,18,"Choose board size",GOLD);
        for(i=0;i<6;i++){
            x=20+(i%3)*(width-40)/3;y=60+(i/3)*90;
            box(x,y,(width-52)/3,76,i==choice?GR_RGB(54,76,88):PANEL);
            snprintf(label,sizeof(label),"%d x %d",s_sizes[i],s_sizes[i]);text(x+16,y+14,label,i==choice?GOLD:FG);
            text(x+16,y+42,"Fresh puzzles",MUTED);
        }
        text(20,height-44,"Arrows: choose   Enter: play   Esc: cancel",MUTED);
    }else if(mode==2){
        static const char *lines[]={"SHIKAKU / rectangles","Cover the board with rectangles or squares.","Each rectangle contains exactly one clue.","The clue equals its area (width times height).","Drag from any corner to draw; overlaps are replaced.","Right-drag or Ctrl/Shift-drag erases whole rectangles.","Red borders mark rectangles that do not fit a clue.","+ / -: zoom   F: fit   Wheel: scroll","Shift + wheel: horizontal scroll","Arrows: cursor   Space: anchor   Enter: draw","Delete: erase   U/Y: undo/redo   N: next puzzle","Progress saves automatically. Esc: return"};
        int gap=(height-40)/12;if(gap>25)gap=25;
        for(i=0;i<12;i++)text(16,16+i*gap,lines[i],i?FG:GOLD);
    }else if(mode==4){
        text(24,70,"Creating a uniquely solvable puzzle...",GOLD);
        snprintf(label,sizeof(label),"%d x %d / seed %lu",generating_size,generating_size,generating_seed);text(24,110,label,FG);
        snprintf(label,sizeof(label),"Refinements: %d",s_generate_refinements(generator));text(24,142,label,MUTED);
        text(24,180,"Checking that there is exactly one solution.",FG);
        text(24,220,"Esc: cancel and keep your current board",MUTED);
    }else{
        text(24,70,"Clear this board?",GOLD);text(24,110,"Your rectangles will be removed. Undo can restore them.",FG);
        box(24,174,140,32,PANEL);text(34,182,"Enter: Clear",GOLD);
        box(184,174,140,32,PANEL);text(194,182,"Esc: Cancel",FG);
    }
}
static void paint(void) {
    int i,bw,errors,covered;char line[128];
    if(!full&&!board_dirty&&!cache_dirty)return;
    if(mode){if(!full)return;if(board_mapped){GrUnmapWindow(board);board_mapped=0;}modal();full=board_dirty=0;GrFlush();return;}
    if(full){
        target=window;box(0,0,width,height,BG);box(0,0,width,78,PANEL);
        text(12,10,"SHIKAKU",GOLD);
        if(game.generated)snprintf(line,sizeof(line),"%d x %d / seed %lu",game.size,game.size,game.seed);
        else snprintf(line,sizeof(line),"%d x %d / Puzzle %d of %d",game.size,game.size,game.puzzle%S_PER_SIZE+1,S_PER_SIZE);text(96,10,line,MUTED);
        covered=s_progress(&game,&errors);
        snprintf(line,sizeof(line),s_won(&game)?"COMPLETE":"%d / %d",covered,game.size*game.size);text(width-112,10,line,s_won(&game)?GREEN:FG);
        bw=(width-20)/9;
        for(i=0;i<9;i++){
            const char *s=buttons[i];GR_COLOR c=FG;
            box(10+i*bw,42,bw-4,27,GR_RGB(50,65,77));
            if(i==2&&!game.cursor||i==3&&game.cursor+1==game.states)c=MUTED;
            if(i==0){snprintf(line,sizeof(line),"%dx%d",game.size,game.size);s=line;}
            text(10+i*bw+(bw-4-(int)strlen(s)*8)/2,48,s,c);
        }
        if(!layout()){stopping=1;exit_code=1;return;}
        target=window;edge(vx-2,vy-2,vw+4,vh+4,MAJOR);edge(vx-1,vy-1,vw+2,vh+2,MAJOR);
        if(extent>vh){int thumb=(int)((long)vh*vh/extent);if(thumb<12)thumb=12;box(vx+vw+5,vy,8,vh,PANEL);box(vx+vw+5,vy+(int)((long)pany*(vh-thumb)/(extent-vh)),8,thumb,MUTED);}
        if(extent>vw){int thumb=(int)((long)vw*vw/extent);if(thumb<12)thumb=12;box(vx,vy+vh+5,vw,8,PANEL);box(vx+(int)((long)panx*(vw-thumb)/(extent-vw)),vy+vh+5,thumb,8,MUTED);}
        box(0,height-44,width,44,PANEL);
        if(dragging){SRect r=selection();snprintf(line,sizeof(line),erasing?"Erase rectangles in %d x %d cells":"%d x %d = %d cells%s",r.w,r.h,r.w*r.h,s_status(&game,r)==1?"  /  clue matched":"");}
        else if(s_won(&game))snprintf(line,sizeof(line),"Beautiful! Board complete. New opens the next puzzle.");
        else if(errors)snprintf(line,sizeof(line),"%d rectangle%s need%s attention.  %s",errors,errors==1?"":"s",errors==1?"s":"",message);
        else snprintf(line,sizeof(line),"%s",message);
        line[(width-20)/8<(int)sizeof(line)?(width-20)/8:(int)sizeof(line)-1]=0;text(10,height-40,line,s_won(&game)?GREEN:dragging?GOLD:MUTED);
        snprintf(line,sizeof(line),"Drag: draw | Right/Ctrl/Shift-drag: erase | Zoom %d%%",tile*100/40);text(10,height-20,line,MUTED);
        if(!board_mapped){GrMapWindow(board);board_mapped=1;}board_dirty=1;
    }
    if(board_dirty||cache_dirty)paint_board();full=0;GrFlush();frames++;
    if(trace)fprintf(stderr,"SHIKAKU_PAINT %lu %d %d %d %d %d\n",frames,tile,vx,vy,vw,vh);
}
static void changed(int result) {
    if(result<0){say("Not enough memory for undo history; board unchanged.");return;}
    if(!result)return;
    unsaved=1;cache_dirty=full=1;
    if(!save_blocked)say("Progress saved automatically.");save();
    if(trace)fprintf(stderr,"SHIKAKU_CHANGE %d %d %d %d\n",game.puzzle,s_state(&game)->count,game.cursor,s_won(&game));
}
static void do_zoom(int delta) {
    int old=tile,next=tile+delta;if(next<10)next=10;if(next>64)next=64;
    panx=(int)(((long)panx+vw/2)*next/old)-vw/2;pany=(int)(((long)pany+vh/2)*next/old)-vh/2;zoom=next;dragging=0;full=1;
}
static void action(int item) {
    if(trace)fprintf(stderr,"SHIKAKU_ACTION %d\n",item);
    dragging=0;
    switch(item){
    case 0:mode=1;choice=game.puzzle/S_PER_SIZE;full=1;break;
    case 1:generate(game.size,fresh_seed());break;
    case 2:changed(s_undo(&game));break;case 3:changed(s_redo(&game));break;
    case 4:mode=3;full=1;break;case 5:mode=2;full=1;break;
    case 6:do_zoom(-4);break;case 7:do_zoom(4);break;case 8:zoom=panx=pany=0;full=1;break;
    }
}
static void see_cursor(void) {
    if(cx*tile<panx)panx=cx*tile;if((cx+1)*tile>=panx+vw)panx=(cx+1)*tile-vw+1;
    if(cy*tile<pany)pany=cy*tile;if((cy+1)*tile>=pany+vh)pany=(cy+1)*tile-vh+1;clamp_pan();full=1;
}
static void key(int ch,unsigned mods) {
    SRect r;
    if(mods&MWKMOD_CTRL){if(ch=='z'||ch=='Z'||ch==26)ch='u';if(ch=='y'||ch=='Y'||ch==25)ch='y';}
    if(ch=='q'||ch=='Q'){stopping=1;return;}
    if(mode){
        if(ch==27||ch==MWKEY_ESCAPE){if(mode==4)cancel_generation();else mode=0;full=1;return;}
        if(mode==4)return;
        if(mode==1){
            if(ch==MWKEY_LEFT)choice=(choice+5)%6;if(ch==MWKEY_RIGHT)choice=(choice+1)%6;
            if(ch==MWKEY_UP||ch==MWKEY_DOWN)choice=(choice+3)%6;
            if(ch=='\r'||ch=='\n')select_size(choice);
        }else if(mode==2){if(ch==MWKEY_F1||ch=='\r'||ch=='\n')mode=0;}
        else if(ch=='\r'||ch=='\n'){mode=0;save_blocked=0;unsaved=1;changed(s_clear(&game));save();}
        full=1;return;
    }
    if(ch==MWKEY_LEFT||ch==MWKEY_RIGHT||ch==MWKEY_UP||ch==MWKEY_DOWN){
        keyboard_cursor=1;if(ch==MWKEY_LEFT&&cx)cx--;if(ch==MWKEY_RIGHT&&cx+1<game.size)cx++;
        if(ch==MWKEY_UP&&cy)cy--;if(ch==MWKEY_DOWN&&cy+1<game.size)cy++;
        if(dragging){ex=cx;ey=cy;}see_cursor();return;
    }
    switch(ch){
    case ' ':if(!dragging){ax=ex=cx;ay=ey=cy;dragging=1;erasing=0;keyboard_cursor=1;}else{ex=cx;ey=cy;}full=1;break;
    case '\r':case '\n':if(dragging){r=selection();dragging=0;changed(s_apply(&game,r,erasing));full=1;}break;
    case 27:dragging=0;full=1;break;
    case MWKEY_DELETE:case 127:case 8:r.x=cx;r.y=cy;r.w=r.h=1;changed(s_apply(&game,r,1));break;
    case 'b':case 'B':action(0);break;case 'n':case 'N':case ']':action(1);break;
    case '[':choose((game.puzzle/S_PER_SIZE)*S_PER_SIZE+(game.puzzle%S_PER_SIZE+S_PER_SIZE-1)%S_PER_SIZE);break;
    case 'u':case 'U':action(2);break;case 'y':case 'Y':action(3);break;
    case 'r':case 'R':action(4);break;case MWKEY_F1:case '?':action(5);break;
    case '-':case '_':action(6);break;case '+':case '=':action(7);break;case 'f':case 'F':action(8);break;
    case MWKEY_PAGEDOWN:pany+=vh*3/4;clamp_pan();full=1;break;
    case MWKEY_PAGEUP:pany-=vh*3/4;clamp_pan();full=1;break;
    case 's':case 'S':save();break;
    }
}
static int point(int x,int y,int clamp,int *xx,int *yy) {
    if(!clamp&&(x<0||y<0||x>=vw||y>=vh))return 0;
    x=(x+panx)/tile;y=(y+pany)/tile;
    if(x<0)x=0;if(y<0)y=0;if(x>=game.size)x=game.size-1;if(y>=game.size)y=game.size-1;*xx=x;*yy=y;return 1;
}
static void down(GR_EVENT_BUTTON *b) {
    int x=b->x,y=b->y,bw=(width-20)/9;
    if(b->changebuttons&(GR_BUTTON_SCROLLUP|GR_BUTTON_SCROLLDN)){
        int delta=b->changebuttons&GR_BUTTON_SCROLLUP?-tile*2:tile*2;
        if(mode)return;
        if(b->modifiers&MWKMOD_CTRL)do_zoom(delta<0?4:-4);
        else {if(b->modifiers&MWKMOD_SHIFT)panx+=delta;else pany+=delta;clamp_pan();full=1;}return;
    }
    if(mode){
        if(!(b->changebuttons&GR_BUTTON_L))return;
        if(mode==2){mode=0;full=1;}
        else if(mode==1&&x>=20&&x<width-20&&y>=60&&y<240){int i=(x-20)/((width-40)/3)+3*((y-60)/90);if(i<6){choice=i;key('\r',0);}}
        else if(mode==3&&y>=174&&y<206){if(x>=24&&x<164)key('\r',0);else if(x>=184&&x<324)key(27,0);}
        return;
    }
    if(b->wid==window){
        if(!(b->changebuttons&GR_BUTTON_L))return;
        if(y>=42&&y<69&&x>=10&&x<10+9*bw){action((x-10)/bw);return;}
        if(x>=vx+vw+4&&x<vx+vw+15&&y>=vy&&y<vy+vh&&extent>vh){scroll_drag=2;pany=(int)((long)(y-vy)*(extent-vh)/vh);clamp_pan();full=1;}
        if(y>=vy+vh+4&&y<vy+vh+15&&x>=vx&&x<vx+vw&&extent>vw){scroll_drag=1;panx=(int)((long)(x-vx)*(extent-vw)/vw);clamp_pan();full=1;}
        return;
    }
    if(b->wid!=board||!(b->changebuttons&(GR_BUTTON_L|GR_BUTTON_R))||!point(x,y,0,&cx,&cy))return;
    keyboard_cursor=0;ax=ex=cx;ay=ey=cy;dragging=1;
    erasing=!!(b->changebuttons&GR_BUTTON_R)||!!(b->modifiers&(MWKMOD_CTRL|MWKMOD_SHIFT));full=1;
}
static void motion(GR_EVENT_MOUSE *m) {
    int x,y;
    if(scroll_drag&&m->wid==window){if(scroll_drag==1)panx=(int)((long)(m->x-vx)*(extent-vw)/vw);else pany=(int)((long)(m->y-vy)*(extent-vh)/vh);clamp_pan();full=1;return;}
    if(!dragging||mode||m->wid!=board)return;
    point(m->x,m->y,1,&x,&y);if(x!=ex||y!=ey){ex=cx=x;ey=cy=y;full=1;}
}
int main(int argc,char **argv) {
    GR_SCREEN_INFO screen;GR_EVENT e;FILE *f;char current[448],buf[32],*end;const char *home=getenv("HOME");int i,p=0,arg=-1,library=0;long n;unsigned long requested_seed=0;
    for(i=1;i<argc;i++){
        if(!strcmp(argv[i],"--help")){puts("usage: shikaku [5|7|10|15|20|25] [--seed 1-4294967295 | --puzzle 1-12]\nDrag: draw. Right/Ctrl/Shift-drag: erase. +/-: zoom. F: fit. B: board size.\nU/Y: undo/redo. N: next. Arrows/Space/Enter: keyboard drawing. F1: help.\nProgress: $HOME/.shikaku (SHIKAKU_HOME overrides). Undo is session-local.");return 0;}
        if(!strcmp(argv[i],"--seed")&&i+1<argc){const char *v=argv[++i];if(*v<'0'||*v>'9')return 2;errno=0;requested_seed=strtoul(v,&end,10);if(errno==ERANGE||*end||!requested_seed||requested_seed>0xffffffffUL)return 2;continue;}
        if(!strcmp(argv[i],"--puzzle")&&i+1<argc){library=1;n=strtol(argv[++i],&end,10);if(*end||n<1||n>S_PER_SIZE)return 2;p=n-1;continue;}
        n=strtol(argv[i],&end,10);if(*end||arg>=0)return 2;
        for(arg=0;arg<6&&s_sizes[arg]!=n;arg++){}if(arg==6)return 2;
    }
    if(library&&requested_seed)return 2;
    trace=!!getenv("SHIKAKU_TRACE");
    n=snprintf(directory,sizeof(directory),"%s%s",getenv("SHIKAKU_HOME")?getenv("SHIKAKU_HOME"):home?home:"/root",getenv("SHIKAKU_HOME")?"":"/.shikaku");
    if(n>=(long)sizeof(directory)||(mkdir(directory,0700)<0&&errno!=EEXIST)){perror("shikaku: save directory");return 1;}
    snprintf(current,sizeof(current),"%s/current",directory);f=fopen(current,"r");
    if(f){if(fgets(buf,sizeof(buf),f)){n=strtol(buf,&end,10);if(n>=0&&n<S_PUZZLES&&(*end=='\n'||!*end))game.puzzle=n;}
        for(i=0;i<6&&fgets(buf,sizeof(buf),f);i++){n=strtol(buf,&end,10);if(n>=0&&n<S_PER_SIZE&&(*end=='\n'||!*end))remembered[i]=n;}
        for(i=0;i<6&&fgets(buf,sizeof(buf),f);i++){unsigned long v=strtoul(buf,&end,10);if(*buf>='0'&&*buf<='9'&&v<=0xffffffffUL&&(*end=='\n'||!*end))remembered_seed[i]=v;}
        fclose(f);}
    i=arg>=0?arg:game.puzzle/S_PER_SIZE;
    if(!requested_seed&&!library)requested_seed=remembered_seed[i];
    restoring=!!requested_seed;choose(i*S_PER_SIZE+(library?p:remembered[i]));restoring=0;
    if(GrOpen()<0){fputs("shikaku: start the graphical environment with startx\n",stderr);s_destroy(&game);return 1;}
    connected=1;GrGetScreenInfo(&screen);width=screen.cols-24;if(width>640)width=640;if(width<480)width=480;
    height=screen.rows-64;if(height>600)height=600;if(height<340)height=340;
    snprintf(current,sizeof(current),"Shikaku / %dx%d / %d",game.size,game.size,game.puzzle%S_PER_SIZE+1);
    window=GrNewWindowEx(GR_WM_PROPS_APPWINDOW,current,GR_ROOT_WINDOW_ID,12,8,width,height,BG);gc=GrNewGC();
    if(!window||!gc){exit_code=1;goto done;}GrSetGCUseBackground(gc,GR_FALSE);
    board=GrNewWindow(window,20,88,100,100,0,PAPER,0);if(!board){exit_code=1;goto done;}
    GrSelectEvents(window,GR_EVENT_MASK_EXPOSURE|GR_EVENT_MASK_CLOSE_REQ|GR_EVENT_MASK_KEY_DOWN|GR_EVENT_MASK_BUTTON_DOWN|GR_EVENT_MASK_BUTTON_UP|GR_EVENT_MASK_MOUSE_POSITION|GR_EVENT_MASK_UPDATE|GR_EVENT_MASK_FOCUS_OUT|GR_EVENT_MASK_TIMER);
    GrSelectEvents(board,GR_EVENT_MASK_EXPOSURE|GR_EVENT_MASK_KEY_DOWN|GR_EVENT_MASK_BUTTON_DOWN|GR_EVENT_MASK_BUTTON_UP|GR_EVENT_MASK_MOUSE_POSITION);
    if(pipe(wakefd)<0||fcntl(wakefd[0],F_SETFL,O_NONBLOCK)<0||fcntl(wakefd[1],F_SETFL,O_NONBLOCK)<0){exit_code=1;goto done;}
    GrRegisterInput(wakefd[0]);signal(SIGTERM,caught);signal(SIGHUP,caught);signal(SIGINT,caught);
    GrMapWindow(window);GrSetFocus(window);paint();if(trace)fprintf(stderr,"SHIKAKU_READY %d %d %d %d %d %d\n",game.puzzle,width,height,tile,vx,vy);
    if(requested_seed)generate(game.size,requested_seed);
    while(!stopping){
        GrGetNextEvent(&e);
        if(e.type==GR_EVENT_TYPE_EXPOSURE){if(e.exposure.wid==board)board_dirty=1;else full=1;}
        else if(e.type==GR_EVENT_TYPE_CLOSE_REQ)stopping=1;
        else if(e.type==GR_EVENT_TYPE_FDINPUT){char bytes[16];read(wakefd[0],bytes,sizeof(bytes));}
        else if(e.type==GR_EVENT_TYPE_TIMER&&generator&&e.timer.tid==generation_timer)generation_step();
        else if(e.type==GR_EVENT_TYPE_KEY_DOWN)key(e.keystroke.ch,e.keystroke.modifiers);
        else if(e.type==GR_EVENT_TYPE_BUTTON_DOWN)down(&e.button);
        else if(e.type==GR_EVENT_TYPE_MOUSE_POSITION)motion(&e.mouse);
        else if(e.type==GR_EVENT_TYPE_BUTTON_UP){
            scroll_drag=0;if(dragging&&(e.button.changebuttons&(GR_BUTTON_L|GR_BUTTON_R))){SRect r;if(e.button.wid==board)point(e.button.x,e.button.y,1,&ex,&ey);r=selection();dragging=0;changed(s_apply(&game,r,erasing));full=1;}
        }else if(e.type==GR_EVENT_TYPE_FOCUS_OUT){dragging=scroll_drag=0;full=1;}
        else if(e.type==GR_EVENT_TYPE_UPDATE&&e.update.utype==GR_UPDATE_SIZE&&e.update.wid==window){
            width=e.update.width;height=e.update.height;dragging=scroll_drag=0;
            if(width<480||height<340){if(width<480)width=480;if(height<340)height=340;GrResizeWindow(window,width,height);}full=1;
            if(trace)fprintf(stderr,"SHIKAKU_RESIZE %d %d\n",width,height);
        }
        if(!stopping)paint();
    }
    save();
done:
    if(generator)cancel_generation();
    if(wakefd[0]>=0){GrUnregisterInput(wakefd[0]);close(wakefd[0]);}if(wakefd[1]>=0)close(wakefd[1]);wakefd[0]=wakefd[1]=-1;
    if(cache)GrDestroyWindow(cache);if(window)GrDestroyWindow(window);if(gc)GrDestroyGC(gc);GrClose();s_destroy(&game);
    if(trace)fprintf(stderr,"SHIKAKU_EXIT %d frames=%lu\n",exit_code,frames);return exit_code;
}
