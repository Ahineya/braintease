/* Small software-rendering/input acceptance application; no font/assets needed. */
#include <SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static SDL_Surface *screen;
static int selected_x = 2, selected_y = 2, tint, trace, depth = 32;
static int full_redraw, profile;
static int painted_x, painted_y, painted_tint, painted_width, painted_height;

static unsigned long microseconds(void)
{
    struct timespec now;
    if(clock_gettime(CLOCK_MONOTONIC,&now)<0){
        fprintf(stderr,"SDL_DEMO clock failed\n");exit(1);
    }
    return (unsigned long)now.tv_sec*1000000UL+(unsigned long)now.tv_nsec/1000UL;
}

static void fill(int x, int y, int w, int h, int r, int g, int b)
{
    SDL_Rect rect;
    rect.x = x; rect.y = y; rect.w = w; rect.h = h;
    if (SDL_FillRect(screen, &rect, SDL_MapRGB(screen->format,r,g,b)) < 0) {
        fprintf(stderr,"SDL_DEMO fill failed: %s\n",SDL_GetError()); exit(1);
    }
}

static void tile(int x,int y,int cw,int ch)
{
    int light=(x+y)&1;
    int px=40+x*cw,py=64+y*ch;
    fill(px,py,cw,ch,light?52:42,light?88:72,light?72:60);
    if((x+3*y)%7==0)
        fill(px+cw/4,py+ch/4,cw/2,ch/2,
             tint?210:132,tint?135:160,tint?88:184);
    if(x==selected_x&&y==selected_y){
        /* A bright outline makes clicks/arrows visible without textual widgets. */
        fill(px,py,cw,3,230,195,100);fill(px,py+ch-3,cw,3,230,195,100);
        fill(px,py,3,ch,230,195,100);fill(px+cw-3,py,3,ch,230,195,100);
    }
}

static void damage(SDL_Rect *rects,int *count,int x,int y,int w,int h)
{
    SDL_Rect *rect=&rects[(*count)++];
    rect->x=x;rect->y=y;rect->w=w;rect->h=h;
}

static int draw(int repaint)
{
    /* At most every tile and both status indicators can change in one frame. */
    SDL_Rect rects[8*6+2];
    SDL_Rect clip;
    int x,y,count=0,cw=(screen->w-80)/8,ch=(screen->h-112)/6;
    int selection=selected_x!=painted_x||selected_y!=painted_y;
    int colors=tint!=painted_tint;
    int full=repaint||full_redraw||screen->w!=painted_width||screen->h!=painted_height;
    unsigned long begin=0,raster=0,end=0,pixels=0;
    if(!full&&!selection&&!colors)return 0;
    if(profile)begin=microseconds();
    SDL_SetClipRect(screen,NULL);
    if(full){
        fill(0,0,screen->w,screen->h,24,28,34);
        fill(0,0,screen->w,42,44,54,68);
        /* Three original geometric icons, also useful visual scaling references. */
        fill(16,12,18,18,100,180,220);fill(48,12,18,18,140,190,130);
        fill(80,12,18,18,220,180,100);
    }
    clip.x=40; clip.y=64; clip.w=cw*8; clip.h=ch*6;
    SDL_SetClipRect(screen,&clip);
    for(y=0;y<6;y++)for(x=0;x<8;x++){
        if(full||(selection&&((x==painted_x&&y==painted_y)||
                              (x==selected_x&&y==selected_y)))||
           (colors&&(x+3*y)%7==0)){
            tile(x,y,cw,ch);
            if(!full)damage(rects,&count,40+x*cw,64+y*ch,cw,ch);
        }
    }
    SDL_SetClipRect(screen,NULL);
    if(full)fill(0,screen->h-26,screen->w,26,44,54,68);
    if(full||selection){
        if(!full){
            fill(16,screen->h-18,8*12,10,44,54,68);
            fill(140,screen->h-18,6*12,10,44,54,68);
            damage(rects,&count,16,screen->h-18,8*12,10);
            damage(rects,&count,140,screen->h-18,6*12,10);
        }
        fill(16,screen->h-18,(selected_x+1)*12,10,100,180,220);
        fill(140,screen->h-18,(selected_y+1)*12,10,140,190,130);
    }
    if(full)damage(rects,&count,0,0,screen->w,screen->h);
    if(profile)raster=microseconds();
    SDL_UpdateRects(screen,count,rects);
    if(profile){
        end=microseconds();
        for(x=0;x<count;x++)pixels+=(unsigned long)rects[x].w*rects[x].h;
        printf("SDL_DEMO profile wall_us=%lu raster_us=%lu present_us=%lu rects=%d pixels=%lu full=%d\n",
               end-begin,raster-begin,end-raster,count,pixels,full);
        fflush(stdout);
    }
    painted_x=selected_x;painted_y=selected_y;painted_tint=tint;
    painted_width=screen->w;painted_height=screen->h;
    if(trace){printf("SDL_DEMO frame %d %d selected %d %d tint %d\n",
        screen->w,screen->h,selected_x,selected_y,tint);fflush(stdout);}
    return 1;
}

static void video(int w,int h)
{
    if(w<320)w=320;
    if(h<240)h=240;
    if(w>1024)w=1024;
    if(h>1024)h=1024;
    screen=SDL_SetVideoMode(w,h,depth,SDL_SWSURFACE|SDL_RESIZABLE);
    if(!screen){fprintf(stderr,"SDL_DEMO mode failed: %s\n",SDL_GetError());exit(1);}
}

int main(int argc,char **argv)
{
    int i,quit=0,dirty=1,repaint=1,frames=0,limit=0,width=640,height=400;
    for(i=1;i<argc;i++){
        if(!strcmp(argv[i],"--trace"))trace=1;
        else if(!strcmp(argv[i],"--frames")&&i+1<argc)limit=atoi(argv[++i]);
        else if(!strcmp(argv[i],"--depth")&&i+1<argc)depth=atoi(argv[++i]);
        else if(!strcmp(argv[i],"--full-redraw"))full_redraw=1;
        else if(!strcmp(argv[i],"--profile"))profile=1;
        else if(!strcmp(argv[i],"--size")&&i+2<argc){width=atoi(argv[++i]);height=atoi(argv[++i]);}
        else{fprintf(stderr,"usage: sdl-demo [--trace] [--frames count] [--depth 16|24|32] [--size width height] [--full-redraw] [--profile]\n");return 2;}
    }
    if(depth!=16&&depth!=24&&depth!=32){fprintf(stderr,"unsupported test depth\n");return 2;}
    if(SDL_Init(SDL_INIT_VIDEO)<0){fprintf(stderr,"SDL_DEMO init failed: %s\n",SDL_GetError());return 1;}
    SDL_WM_SetCaption("SDL software test",NULL);video(width,height);
    while(!quit){
        SDL_Event event;
        int pending=0;
        if(!dirty&&!limit){
            if(!SDL_WaitEvent(&event)){
                fprintf(stderr,"SDL_DEMO wait failed: %s\n",SDL_GetError());
                SDL_Quit();return 1;
            }
            pending=1;
        }
        while(!quit){
            if(!pending&&!SDL_PollEvent(&event))break;
            pending=0;
            if(trace){printf("SDL_DEMO event %u\n",(unsigned)event.type);fflush(stdout);}
            switch(event.type){
            case SDL_QUIT:quit=1;break;
            case SDL_VIDEORESIZE:video(event.resize.w,event.resize.h);dirty=1;repaint=1;break;
            case SDL_VIDEOEXPOSE:dirty=1;repaint=1;break;
            case SDL_KEYDOWN:
                if(event.key.keysym.sym==SDLK_ESCAPE)quit=1;
                else if(event.key.keysym.sym==SDLK_LEFT)selected_x=(selected_x+7)%8;
                else if(event.key.keysym.sym==SDLK_RIGHT)selected_x=(selected_x+1)%8;
                else if(event.key.keysym.sym==SDLK_UP)selected_y=(selected_y+5)%6;
                else if(event.key.keysym.sym==SDLK_DOWN)selected_y=(selected_y+1)%6;
                else if(event.key.keysym.sym==SDLK_SPACE)tint=!tint;
                else break;
                dirty=1;break;
            case SDL_MOUSEBUTTONDOWN:
                if(event.button.button==SDL_BUTTON_LEFT){
                    int cw=(screen->w-80)/8,ch=(screen->h-112)/6;
                    int x=event.button.x,y=event.button.y;
                    if(x>=40&&x<40+cw*8&&y>=64&&y<64+ch*6){
                        selected_x=(x-40)/cw;selected_y=(y-64)/ch;
                    }
                }else if(event.button.button==SDL_BUTTON_WHEELUP)tint=0;
                else if(event.button.button==SDL_BUTTON_WHEELDOWN)tint=1;
                else break;
                dirty=1;break;
            default:break;
            }
        }
        if(dirty&&!quit){
            if(draw(repaint)){frames++;if(limit&&frames>=limit)quit=1;}
            dirty=0;repaint=0;
        }
        if(limit){dirty=1;repaint=1;if(!quit)SDL_Delay(33);}
    }
    SDL_Quit();printf("SDL_DEMO quit frames=%d\n",frames);return 0;
}
