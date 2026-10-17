/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Platform policy for the pinned nxroach. Movement/turning and the original
 * bitmap artwork remain upstream code. Drawing uses transparent C16 masks;
 * GX performs the normal MMU-aware DMA upload of damaged screen rectangles. */
#include <fcntl.h>
static unsigned long roach_frame;
static int roach_trace;
static int roach_wake[2]={-1,-1};
static void nxr_roach_signal(void) {
    int saved=errno;char byte=1;
    done=GR_TRUE;
    /* A queued byte also covers a signal arriving immediately before select.
     * Nonblocking write and errno preservation keep this signal-safe. */
    if(roach_wake[1]>=0)write(roach_wake[1],&byte,1);
    errno=saved;
}
static void nxr_roach_close_wake(void) {
    int writer=roach_wake[1];roach_wake[1]=-1;
    if(roach_wake[0]>=0){GrUnregisterInput(roach_wake[0]);close(roach_wake[0]);roach_wake[0]=-1;}
    if(writer>=0)close(writer);
}
static unsigned long roach_milliseconds(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC,&now);
    return now.tv_sec*1000UL+now.tv_nsec/1000000UL;
}
static void nxr_roach_usage(void) {
    puts("usage: nxroach [-roaches 1..64] [-speed 0..48] [-rc RGB] [-seed 0..32767]\n"
         "Roaches scurry on the desktop and hide under windows.\n"
         "Right-click the bare desktop or press Ctrl-C in the launching terminal to stop.\n"
         "RGB is a numeric Nano-X colour (0xBBGGRR); the default is black.");
}
static void nxr_roach_clear(void) {
    int i;
    if(!roaches)return;
    for(i=0;i<curRoaches;i++)if(roaches[i].intX>=0)
        GrClearArea(GR_ROOT_WINDOW_ID,roaches[i].intX,roaches[i].intY,
                    roaches[i].rp->width,roaches[i].rp->height,GR_FALSE);
}
static void nxr_roach_draw(void) {
    int i;
    nxr_roach_clear();
    ++roach_frame;
    for(i=0;i<curRoaches;i++) {
        Roach *r=&roaches[i];
        if(r->Hidden){r->intX=-1;continue;}
        r->rp=&roachPix[r->index];r->intX=(int)r->x;r->intY=(int)r->y;
        GrBitmap(GR_ROOT_WINDOW_ID,gc,r->intX,r->intY,r->rp->width,r->rp->height,r->rp->roachBits);
        if(roach_trace&&roach_frame%20==1)
            fprintf(stderr,"NXROACH_FRAME %lu %d %d %d %d\n",roach_frame,i,r->index,r->intX,r->intY);
    }
    GrFlush();
}
static int nxr_roach_visible(void) {
    GR_REGION_ID covered=0,visible=0;
    GR_WINDOW_ID parent,*children=NULL;
    GR_COUNT count=0,i;
    GR_WINDOW_INFO info;
    GR_RECT rect;
    GrQueryTree(GR_ROOT_WINDOW_ID,&parent,&children,&count);
    if(count<0||(count&&!children))return -1;
    covered=GrNewRegion();visible=GrNewRegion();
    if(!covered||!visible){free(children);if(covered)GrDestroyRegion(covered);if(visible)GrDestroyRegion(visible);return -1;}
    for(i=0;i<count;i++) {
        GrGetWindowInfo(children[i],&info);
        if(info.realized&&!info.inputonly) {
            rect.x=info.x-info.bordersize;rect.y=info.y-info.bordersize;
            rect.width=info.width+2*info.bordersize;rect.height=info.height+2*info.bordersize;
            GrUnionRectWithRegion(covered,&rect);
        }
    }
    free(children);
    rect.x=rect.y=0;rect.width=display_width;rect.height=display_height;
    GrUnionRectWithRegion(visible,&rect);GrSubtractRegion(visible,visible,covered);GrDestroyRegion(covered);
    if(rootVisible)GrDestroyRegion(rootVisible);
    rootVisible=visible;
    for(i=0;i<curRoaches;i++)roaches[i].Hidden=0;
    return 0;
}
static int nxr_roach_main(int argc,char **argv) {
    int a,i,visible,last_visible=-1,need_calc=1,redraw,result=0;
    unsigned seed=(unsigned)time(NULL);
    unsigned long value,next,now,delay;
    char *end;
    GR_SCREEN_INFO screen;
    GR_EVENT event;
    if(argc==2&&(!strcmp(argv[1],"--help")||!strcmp(argv[1],"-h"))){nxr_roach_usage();return 0;}
    for(a=1;a<argc;a++) {
        char *option=argv[a];
        if(++a==argc){nxr_roach_usage();return 2;}
        errno=0;
        if(!strcmp(option,"-speed")) {
            double speed=strtod(argv[a],&end);
            if(errno||end==argv[a]||*end||!(speed>=0.0&&speed<=48.0)){nxr_roach_usage();return 2;}
            roachSpeed=(float)speed;
        } else {
            value=strtoul(argv[a],&end,0);
            if(errno||end==argv[a]||*end||argv[a][0]=='-'){nxr_roach_usage();return 2;}
            if(!strcmp(option,"-roaches")){if(value<1||value>64){nxr_roach_usage();return 2;}maxRoaches=(int)value;}
            else if(!strcmp(option,"-rc")){if(value>0xffffffUL){nxr_roach_usage();return 2;}roachColor=value;}
            else if(!strcmp(option,"-seed")){if(value>32767UL){nxr_roach_usage();return 2;}seed=(unsigned)value;}
            else {nxr_roach_usage();return 2;}
        }
    }
    srand(seed);roach_trace=getenv("NANOX_TRACE")!=NULL;
    if(GrOpen()<0){fputs("nxroach: cannot connect to Nano-X\n",stderr);return 1;}
    GrGetScreenInfo(&screen);display_width=screen.cols;display_height=screen.rows;
    if(display_width<=48||display_height<=48){GrClose();return 1;}
    for(i=0;i<ROACH_HEADINGS;i++) {
        float angle=i*0.261799387799;
        roachPix[i].sine=sin(angle);roachPix[i].cosine=cos(angle);
    }
    roaches=calloc(maxRoaches,sizeof(*roaches));gc=GrNewGC();
    if(!roaches||!gc){free(roaches);GrClose();return 1;}
    if(pipe(roach_wake)<0||fcntl(roach_wake[0],F_SETFL,O_NONBLOCK)<0||fcntl(roach_wake[1],F_SETFL,O_NONBLOCK)<0) {
        perror("nxroach wake");nxr_roach_close_wake();free(roaches);GrClose();return 1;
    }
    GrRegisterInput(roach_wake[0]);
    GrSetGCForeground(gc,roachColor);GrSetGCUseBackground(gc,GR_FALSE);
    while(curRoaches<maxRoaches)AddRoach();
    signal(SIGINT,SigHandler);signal(SIGTERM,SigHandler);signal(SIGHUP,SigHandler);
    GrSelectEvents(GR_ROOT_WINDOW_ID,GR_EVENT_MASK_EXPOSURE|GR_EVENT_MASK_CHLD_UPDATE|GR_EVENT_MASK_BUTTON_DOWN);
    GrFlush();
    if(roach_trace)fprintf(stderr,"NXROACH_READY %d %d\n",getpid(),maxRoaches);
    next=roach_milliseconds();
    while(!done) {
        redraw=need_calc;
        if(need_calc){if(CalcRootVisible()<0){fputs("nxroach: cannot calculate visible desktop\n",stderr);result=1;break;}need_calc=0;}
        visible=MarkHiddenRoaches();
        now=roach_milliseconds();
        if(visible&&(redraw||(roachSpeed>0&&(long)(now-next)>=0))) {
            if(roachSpeed>0)for(i=0;i<curRoaches;i++)if(!roaches[i].Hidden)MoveRoach(i);
            DrawRoaches();next=roach_milliseconds()+50;
        } else if(!visible&&last_visible>0)DrawRoaches();
        if(roach_trace&&visible!=last_visible)fprintf(stderr,"NXROACH_VISIBLE %d\n",visible);
        last_visible=visible;
        if(done)break;
        now=roach_milliseconds();delay=(long)(next-now)>0?next-now:1;
        GrGetNextEventTimeout(&event,visible&&roachSpeed>0?delay:GR_TIMEOUT_BLOCK);
        if(event.type==GR_EVENT_TYPE_EXPOSURE||event.type==GR_EVENT_TYPE_CHLD_UPDATE)need_calc=1;
        if(event.type==GR_EVENT_TYPE_BUTTON_DOWN&&(event.button.buttons&GR_BUTTON_R)) {
            GR_WINDOW_ID under;GR_COORD x,y;GR_BUTTON buttons;
            GrQueryPointer(&under,&x,&y,&buttons);
            if(under==GR_ROOT_WINDOW_ID)done=GR_TRUE;
        }
    }
    CoverRoot();GrGetScreenInfo(&screen); /* finish erasing before reporting exit */
    nxr_roach_close_wake();
    if(rootVisible)GrDestroyRegion(rootVisible);
    GrDestroyGC(gc);free(roaches);roaches=NULL;
    if(roach_trace)fprintf(stderr,"NXROACH_EXIT %d\n",getpid());
    GrClose();return result;
}
