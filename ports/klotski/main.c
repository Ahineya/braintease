/* SPDX-License-Identifier: GPL-3.0-or-later */
/* GNOME Klotski puzzles/rules/art, Nano-X presentation for Riplix and Fuse-X. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include "nano-X.h"
#include "fde_nano.h"
#include "klotski.h"
#include "tiles.inc"
#define BG GR_RGB(26,33,39)
#define PANEL GR_RGB(35,44,53)
#define FG GR_RGB(226,233,235)
#define MUTED GR_RGB(151,170,180)
#define GOLD GR_RGB(235,193,110)
#define GREEN GR_RGB(153,223,118)
static KGame game;
static unsigned long best[K_LEVELS],frames;
static unsigned revision,painted_revision;
static GR_WINDOW_ID window,atlas;
static GR_GC_ID gc;
static FDE_IMAGE source;
static int connected,width=600,height=440,tile,bx,by,level,selected,dragging,px,py;
static int full=1,hud=1,mode,group,choice,top,save_blocked,trace,exit_code;
static int wakefd[2]={-1,-1};
static volatile int stopping;
static char directory[384],path[448],message[96],painted[K_CELLS];
static int painted_tile[K_CELLS];
static char old_selected;
static const char *groups[]={"Huarong Trail","Challenge","Skill"};
static const char *buttons[]={"Puzzles","<",">","Restart","Undo","Redo","Help"};
static void box(int x,int y,int w,int h,GR_COLOR c)
{ if(w>0&&h>0){GrSetGCForeground(gc,c);GrFillRect(window,gc,x,y,w,h);} }
static void text(int x,int y,const char *s,GR_COLOR c)
{
    int n=strlen(s),limit=(width-x-8)/8;if(n>limit)n=limit;
    if(n>0){GrSetGCForeground(gc,c);GrText(window,gc,x,y,s,n,GR_TFASCII|GR_TFTOP);}
}
static void say(const char *s) { snprintf(message,sizeof(message),"%s",s);hud=1; }
static void caught(int sig)
{
    int e=errno;char c=1;signal(sig,caught);stopping=1;
    if(wakefd[1]>=0)write(wakefd[1],&c,1);errno=e;
}
static void level_path(int n,char *out,unsigned size)
{ snprintf(out,size,"%s/level-%02d",directory,n+1); }
static void remember(void)
{
    FILE *f;char name[448],temp[480];int failed=0;
    snprintf(name,sizeof(name),"%s/current",directory);snprintf(temp,sizeof(temp),"%s.tmp-%d",name,(int)getpid());
    f=fopen(temp,"w");if(!f)return;
    if(fprintf(f,"%d\n",level+1)<0||fflush(f)||k_flush(fileno(f)))failed=1;
    if(fclose(f))failed=1;
    if(!failed&&rename(temp,name)==0)return;
    unlink(temp);
}
static int save(void)
{
    if(save_blocked)return 0;
    if(k_won(&game)&&(!best[level]||game.score.moves<best[level]))best[level]=game.score.moves;
    if(k_save(&game,path,best[level])<0){
        perror("klotski: save");say("Save failed; progress remains in memory. S: retry.");exit_code=1;return 0;
    }
    say(k_won(&game)?"Puzzle solved! Your best score is saved.":"Progress saved.");
    if(trace)fprintf(stderr,"KLOTSKI_SAVE %d %u %u %lu\n",level+1,game.count,game.cursor,best[level]);
    return 1;
}
static void choose(int n)
{
    GR_WM_PROPERTIES p;char title[100];int result;
    level=n;group=k_levels[n].group;choice=n;top=0;selected=dragging=0;save_blocked=0;
    k_begin(&game,k_levels+n);level_path(n,path,sizeof(path));
    result=k_load(&game,path,best+n);
    if(result<0){save_blocked=1;say("Save unreadable; R then Enter starts fresh. File kept.");perror("klotski: load");}
    else say(result?"Saved puzzle restored.":"Slide the marked block onto the green target.");
    if(connected){
        memset(&p,0,sizeof(p));snprintf(title,sizeof(title),"Klotski / %s",k_levels[n].name);
        p.flags=GR_WM_FLAGS_TITLE;p.title=title;GrSetWMProperties(window,&p);
    }
    remember();full=hud=1;
    if(trace)fprintf(stderr,"KLOTSKI_LEVEL %d %lu %u\n",level+1,game.score.moves,game.cursor);
}
static int layout(void)
{
    int s=(width-32)/game.level->width,t=(height-140)/game.level->height,i;
    struct fde_rect rects[9];GR_WINDOW_ID next;
    if(t<s)s=t;if(s>48)s=48;if(s<2)s=2;s&=~1;
    bx=(width-game.level->width*s)/2;by=88+(height-132-game.level->height*s)/2;
    if(s==tile&&atlas)return 1;
    next=GrNewPixmapEx(s*9,s*3,MWIF_RGB565,NULL);if(!next)return 0;
    for(i=0;i<27;i++){
        struct fde_rect *r=rects+i%9;
        r->x=(i%9)*s;r->y=(i/9)*s;r->sx=(i%9)*34;r->sy=(i/9)*34;
        r->sw=r->sh=34;r->width=r->height=s;
        if(i%9==8&&fde_image_draw(source,next,gc,rects,9,0)<0){GrDestroyWindow(next);return 0;}
    }
    if(atlas)GrDestroyWindow(atlas);atlas=next;tile=s;return 1;
}
static void border(int x,int y,int w,int h,GR_COLOR c)
{ GrSetGCForeground(gc,c);GrRect(window,gc,x,y,w,h); }
static void draw_cell(int x,int y)
{
    int number=k_tile(&game,x,y),xx=bx+x*tile,yy=by+y*tile;char c=k_at(&game,x,y);
    if(number<0){say("Unsupported tile shape.");exit_code=1;stopping=1;return;}
    GrCopyArea(window,gc,xx,yy,tile,tile,atlas,(number%9)*tile,(number/9)*tile,MWROP_COPY);
    if(c=='*'){
        int size=8*tile/34;if(size<2)size=2;
        box(xx+(tile-size)/2,yy+(tile-size)/2,size,size,game.level->map[y*game.level->width+x]=='.'?GREEN:GOLD);
    }
    if(c==selected)border(xx+1,yy+1,tile-2,tile-2,GOLD);
}
static int rows(void) { int n=(height-110)/26;return n<1?1:n; }
static int group_first(int g) { int i;for(i=0;i<K_LEVELS;i++)if(k_levels[i].group==g)return i;return 0; }
static int group_end(int g) { int i=group_first(g);while(i<K_LEVELS&&k_levels[i].group==g)i++;return i; }
static void picker_layout(void)
{
    int first=group_first(group),n=choice-first;
    if(n<top)top=n;if(n>=top+rows())top=n-rows()+1;
    if(top<0)top=0;
}
static void modal(void)
{
    int i,n,start=group_first(group)+top;char line[100];
    box(0,0,width,height,BG);
    if(mode==1){
        for(i=0;i<3;i++){
            box(i*width/3+4,8,width/3-8,28,i==group?GR_RGB(62,83,96):PANEL);
            text(i*width/3+12,14,groups[i],i==group?GOLD:FG);
        }
        picker_layout();start=group_first(group)+top;
        for(i=0;i<rows()&&start+i<group_end(group);i++){
            n=start+i;if(n==choice)box(12,48+i*26,width-24,25,GR_RGB(51,72,84));
            snprintf(line,sizeof(line),"%2d  %-22s",n+1,k_levels[n].name);text(20,52+i*26,line,FG);
            if(best[n])snprintf(line,sizeof(line),"Best %lu",best[n]);else strcpy(line,"Unsolved");
            text(width-132,52+i*26,line,best[n]?GREEN:MUTED);
        }
        text(16,height-48,"Left/Right: group   Up/Down: puzzle",MUTED);
        text(16,height-28,"Enter/click: play   Esc: return",GOLD);
    }else if(mode==2){
        static const char *help[]={"KLOTSKI / GNOME sliding block puzzles","Move the marked block onto the green target.","Drag a piece with the mouse.","Tab / Shift-Tab: select a piece; arrows: slide.","C: puzzles   [ / ]: previous / next puzzle","U / Ctrl-Z: undo   Y / Ctrl-Y: redo","R: restart (confirmation)   S: save again","Each piece's consecutive slides count as one move.","Progress and best scores are saved per puzzle.","GNOME Klotski 3.38.2; GPL-3.0-or-later.","Lars Rydlinge, Robert Ancell, John Cheetham.","Esc / F1 / click: return"};
        for(i=0;i<12;i++)text(18,18+i*24,help[i],i==0||i==11?GOLD:FG);
    }else{
        text(24,72,"Restart this puzzle?",GOLD);text(24,112,"The current board and undo history will be reset.",FG);
        text(24,140,"Your best score will be kept.",FG);
        box(24,190,128,30,PANEL);text(34,197,"Enter: Restart",GOLD);
        box(172,190,128,30,PANEL);text(182,197,"Esc: Cancel",FG);
    }
}
static void paint(void)
{
    int x,y,i,n,t,changed=0;char line[160];
    if(mode){if(full){modal();full=0;GrFlush();frames++;if(trace)fprintf(stderr,"KLOTSKI_PAINT %d %d %d\n",mode,level+1,tile);}return;}
    if(!full&&!hud&&selected==old_selected&&revision==painted_revision)return;
    if(full){
        if(!layout()){fputs("klotski: cannot allocate tile cache\n",stderr);exit_code=1;stopping=1;return;}
        box(0,0,width,height,BG);hud=1;changed=1;
        border(bx-2,by-2,tile*game.level->width+4,tile*game.level->height+4,MUTED);
    }
    if(full||hud){
        box(0,0,width,82,PANEL);
        snprintf(line,sizeof(line),"%s",game.level->name);text(12,9,line,GOLD);
        snprintf(line,sizeof(line),"Moves %lu",game.score.moves);text(width-132,9,line,FG);
        if(best[level])snprintf(line,sizeof(line),"%s   |   Best %lu%s",groups[k_levels[level].group],best[level],k_won(&game)?"   COMPLETE":"");
        else snprintf(line,sizeof(line),"%s%s",groups[k_levels[level].group],k_won(&game)?"   |   COMPLETE":"");
        text(12,30,line,k_won(&game)?GREEN:MUTED);
        for(i=0;i<7;i++){
            int bw=(width-16)/7;box(8+i*bw,53,bw-3,24,GR_RGB(50,65,77));text(12+i*bw,57,buttons[i],FG);
        }
        box(0,height-40,width,40,PANEL);text(10,height-36,message,k_won(&game)?GREEN:MUTED);
        text(10,height-18,"Tab: piece  Arrows: slide  C: puzzles  F1: help  Q: quit",MUTED);changed=1;
    }
    for(y=0;y<game.level->height;y++)for(x=0;x<game.level->width;x++){
        n=y*game.level->width+x;
        /* An overlapping cell of a translated piece can change from an end
         * into a middle tile without changing its piece ID. Compare both. */
        t=k_tile(&game,x,y);
        if(full||painted[n]!=game.board[n]||painted_tile[n]!=t||(old_selected!=selected&&(game.board[n]==old_selected||game.board[n]==selected))){draw_cell(x,y);changed=1;}
        painted_tile[n]=t;
    }
    memcpy(painted,game.board,game.level->width*game.level->height);old_selected=selected;
    full=hud=0;painted_revision=revision;if(changed){GrFlush();frames++;if(trace)fprintf(stderr,"KLOTSKI_PAINT 0 %d %d\n",level+1,tile);}
}
static void moved(void)
{
    hud=1;revision++;if(trace)fprintf(stderr,"KLOTSKI_MOVE %d %lu %u %d\n",level+1,game.score.moves,game.cursor,k_won(&game));
}
static int slide(int dx,int dy)
{
    int result=k_move(&game,(char)selected,dx,dy);
    if(result==1){px+=dx;py+=dy;moved();}
    else if(result<0)say("Undo history is full or memory is exhausted. Undo or restart.");
    return result;
}
static void select_piece(int reverse)
{
    char ids[K_CELLS];int i,j,n=0,index=-1,cells=game.level->width*game.level->height;
    for(i=0;i<cells;i++)if(!k_static(game.board[i])){
        for(j=0;j<n&&ids[j]!=game.board[i];j++){}
        if(j==n)ids[n++]=game.board[i];
    }
    for(i=0;i<n;i++)if(ids[i]==selected)index=i;
    if(!n)return;
    if(reverse)index=index<0?n-1:(index+n-1)%n;else index=(index+1)%n;
    selected=ids[index];
    for(i=0;i<cells;i++)if(game.board[i]==selected){px=i%game.level->width;py=i/game.level->width;break;}
}
static void action(int a)
{
    dragging=0;
    if(a==0){mode=1;choice=level;group=k_levels[level].group;top=0;full=1;}
    else if(a==1||a==2){int n=level+(a==1?-1:1);if(n>=0&&n<K_LEVELS){if(!save_blocked&&!save())return;choose(n);}}
    else if(a==3){mode=3;full=1;}
    else if(a==4||a==5){if(a==4?k_undo(&game):k_redo(&game)){moved();save();}}
    else if(a==6){mode=2;full=1;}
}
static void key(int ch,unsigned mods)
{
    if(mods&MWKMOD_CTRL){if(ch=='z'||ch=='Z'||ch==26)ch='u';else if(ch=='y'||ch=='Y'||ch==25)ch='y';}
    if(ch=='q'||ch=='Q'){stopping=1;return;}
    if(mode){
        if(ch==27||ch==MWKEY_ESCAPE){mode=0;full=1;return;}
        if(mode==2){if(ch==MWKEY_F1||ch=='?'||ch=='\r'||ch=='\n'){mode=0;full=1;}return;}
        if(mode==3){if(ch=='\r'||ch=='\n'||ch=='y'){k_begin(&game,k_levels+level);selected=0;save_blocked=0;mode=0;full=1;save();}return;}
        if(ch==MWKEY_LEFT||ch==MWKEY_RIGHT){group=(group+(ch==MWKEY_LEFT?2:1))%3;choice=group_first(group);top=0;}
        else if(ch==MWKEY_UP&&choice>group_first(group))choice--;
        else if(ch==MWKEY_DOWN&&choice+1<group_end(group))choice++;
        else if(ch=='\r'||ch=='\n'){if(!save_blocked&&!save())return;mode=0;choose(choice);}
        full=1;return;
    }
    if(ch=='\t'){select_piece(mods&MWKMOD_SHIFT);return;}
    if(ch==MWKEY_LEFT||ch==MWKEY_RIGHT||ch==MWKEY_UP||ch==MWKEY_DOWN){
        if(!selected)select_piece(0);
        if(slide(ch==MWKEY_LEFT?-1:ch==MWKEY_RIGHT?1:0,ch==MWKEY_UP?-1:ch==MWKEY_DOWN?1:0)==1)save();
        return;
    }
    switch(ch){
    case 'c':case 'C':action(0);break;case '[':action(1);break;case ']':action(2);break;
    case 'r':case 'R':action(3);break;case 'u':case 'U':action(4);break;case 'y':case 'Y':action(5);break;
    case MWKEY_F1:case '?':action(6);break;case 's':case 'S':save();break;
    case 27:selected=dragging=0;break;
    default:break;
    }
}
static void click(int x,int y)
{
    int n;
    if(mode==2){mode=0;full=1;return;}
    if(mode==3){if(y>=190&&y<220){if(x>=24&&x<152)key('\r',0);else if(x>=172&&x<300)key(27,0);}return;}
    if(mode==1){
        if(y>=8&&y<36){group=x*3/width;if(group>2)group=2;choice=group_first(group);top=0;full=1;}
        else if(y>=48&&y<48+rows()*26){n=group_first(group)+top+(y-48)/26;if(n<group_end(group)){choice=n;key('\r',0);}}
        return;
    }
    if(y>=53&&y<77&&x>=8&&x<width-8){action((x-8)/((width-16)/7));return;}
    if(x<bx||y<by||x>=bx+game.level->width*tile||y>=by+game.level->height*tile)return;
    px=(x-bx)/tile;py=(y-by)/tile;n=k_at(&game,px,py);
    selected=k_static((char)n)?0:n;dragging=!!selected;
}
static void motion(int x,int y)
{
    int nx,ny,dx,dy;
    if(!dragging||mode)return;
    if(x<bx||y<by||x>=bx+game.level->width*tile||y>=by+game.level->height*tile)return;
    nx=(x-bx)/tile;ny=(y-by)/tile;
    /* Coalesced mouse events can cross several cells; replay legal unit slides
     * without teleporting across blockers. Keep the anchor fixed on failure. */
    while(nx!=px||ny!=py){
        dx=nx>px?1:nx<px?-1:0;dy=ny>py?1:ny<py?-1:0;
        if(dx&&dy){if(abs(nx-px)>=abs(ny-py))dy=0;else dx=0;}
        if(k_can(&game,(char)selected,dx,dy)&&!k_won(&game)){
            int before=game.cursor;slide(dx,dy);if(game.cursor==before)break;
        }else if(k_at(&game,px+dx,py+dy)==selected){px+=dx;py+=dy;}
        else break;
    }
}
int main(int argc,char **argv)
{
    GR_SCREEN_INFO screen;GR_EVENT e;FILE *f;
    char current[448],buf[32],*end;const char *home=getenv("HOME");int i,arg=-1,n;long value;
    for(i=1;i<argc;i++){
        if(!strcmp(argv[i],"--list")){for(n=0;n<K_LEVELS;n++)printf("%2d  %-14s %s\n",n+1,groups[k_levels[n].group],k_levels[n].name);return 0;}
        if(!strcmp(argv[i],"--help")){puts("usage: klotski [1-28] [--list]\nMouse: drag. Tab: select. Arrows: slide. C: puzzles. U/Y: undo/redo. F1: help.\nProgress is saved in $HOME/.klotski (KLOTSKI_HOME overrides this directory).");return 0;}
        value=strtol(argv[i],&end,10);if(*end||value<1||value>K_LEVELS||arg>=0){fputs("klotski: expected one puzzle number, 1-28\n",stderr);return 2;}arg=value-1;
    }
    trace=!!getenv("KLOTSKI_TRACE");
    n=snprintf(directory,sizeof(directory),"%s%s",getenv("KLOTSKI_HOME")?getenv("KLOTSKI_HOME"):home?home:"/root",getenv("KLOTSKI_HOME")?"":"/.klotski");
    if(n>=(int)sizeof(directory)||mkdir(directory,0700)<0&&errno!=EEXIST){perror("klotski: save directory");return 1;}
    snprintf(current,sizeof(current),"%s/current",directory);f=fopen(current,"r");
    if(f){if(fgets(buf,sizeof(buf),f)){value=strtol(buf,&end,10);if(value>=1&&value<=K_LEVELS&&(*end=='\n'||!*end))level=value-1;}fclose(f);}
    if(arg>=0)level=arg;
    for(i=0;i<K_LEVELS;i++){level_path(i,current,sizeof(current));best[i]=k_best(k_levels+i,current);}
    choose(level);
    if(GrOpen()<0){fputs("klotski: start the graphical environment with startx\n",stderr);k_free(&game);return 1;}
    connected=1;GrGetScreenInfo(&screen);
    width=screen.cols-24;if(width>600)width=600;if(width<480)width=480;
    height=screen.rows-64;if(height>480)height=480;if(height<340)height=340;
    snprintf(current,sizeof(current),"Klotski / %s",game.level->name);
    window=GrNewWindowEx(GR_WM_PROPS_APPWINDOW,current,GR_ROOT_WINDOW_ID,8,8,width,height,BG);gc=GrNewGC();
    if(!window||!gc){exit_code=1;goto done;}
    GrSetGCUseBackground(gc,GR_FALSE);source=fde_image_load(306,102,k_tiles,NULL);
    if(!source){fputs("klotski: could not upload tile artwork\n",stderr);exit_code=1;goto done;}
    GrSelectEvents(window,GR_EVENT_MASK_EXPOSURE|GR_EVENT_MASK_CLOSE_REQ|GR_EVENT_MASK_KEY_DOWN|
        GR_EVENT_MASK_BUTTON_DOWN|GR_EVENT_MASK_BUTTON_UP|GR_EVENT_MASK_MOUSE_POSITION|GR_EVENT_MASK_UPDATE|GR_EVENT_MASK_FOCUS_OUT);
    if(pipe(wakefd)<0||fcntl(wakefd[0],F_SETFL,O_NONBLOCK)<0||fcntl(wakefd[1],F_SETFL,O_NONBLOCK)<0){exit_code=1;goto done;}
    GrRegisterInput(wakefd[0]);signal(SIGTERM,caught);signal(SIGHUP,caught);signal(SIGINT,caught);
    GrMapWindow(window);GrSetFocus(window);paint();
    if(trace)fprintf(stderr,"KLOTSKI_READY %d %d %d %d %d %d\n",level+1,width,height,tile,bx,by);
    while(!stopping){
        GrGetNextEvent(&e);
        if(e.type==GR_EVENT_TYPE_EXPOSURE)full=1;
        else if(e.type==GR_EVENT_TYPE_CLOSE_REQ)stopping=1;
        else if(e.type==GR_EVENT_TYPE_FDINPUT){char bytes[16];read(wakefd[0],bytes,sizeof(bytes));}
        else if(e.type==GR_EVENT_TYPE_KEY_DOWN)key(e.keystroke.ch,e.keystroke.modifiers);
        else if(e.type==GR_EVENT_TYPE_BUTTON_DOWN&&(e.button.changebuttons&GR_BUTTON_L))click(e.button.x,e.button.y);
        else if(e.type==GR_EVENT_TYPE_BUTTON_UP&&(e.button.changebuttons&GR_BUTTON_L)){if(dragging){motion(e.button.x,e.button.y);dragging=0;save();}}
        else if(e.type==GR_EVENT_TYPE_MOUSE_POSITION)motion(e.mouse.x,e.mouse.y);
        else if(e.type==GR_EVENT_TYPE_FOCUS_OUT){if(dragging)save();dragging=0;}
        else if(e.type==GR_EVENT_TYPE_UPDATE&&e.update.utype==GR_UPDATE_SIZE){
            width=e.update.width;height=e.update.height;dragging=0;
            if(width<480||height<340){if(width<480)width=480;if(height<340)height=340;GrResizeWindow(window,width,height);}
            full=1;if(trace)fprintf(stderr,"KLOTSKI_RESIZE %d %d\n",width,height);
        }
        if(!stopping)paint();
    }
    if(!save_blocked)save();
done:
    if(wakefd[0]>=0){GrUnregisterInput(wakefd[0]);close(wakefd[0]);}if(wakefd[1]>=0)close(wakefd[1]);
    wakefd[0]=wakefd[1]=-1;
    if(atlas)GrDestroyWindow(atlas);if(source)fde_image_destroy(source);
    if(window)GrDestroyWindow(window);if(gc)GrDestroyGC(gc);GrClose();k_free(&game);
    if(trace)fprintf(stderr,"KLOTSKI_EXIT %d frames=%lu\n",exit_code,frames);return exit_code;
}
