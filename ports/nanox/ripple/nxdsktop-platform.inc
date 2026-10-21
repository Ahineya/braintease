/* SPDX-License-Identifier: GPL-2.0-or-later */
/* NXDSKTOP lifecycle, safe argv launching and asynchronous dialog results. */
static int wakefd[2]={-1,-1},modal_fd=-1,modal_kind;
static int children[16],trace,connected,cleaned,modal_bad;
static volatile int stopping;
static char result_line[256],status_text[80];
static unsigned result_count;
static GR_TIMER_ID status_timer;

static void desktop_signal(int sig)
{
    int saved=errno;char byte=1;
    signal(sig,desktop_signal);
    if(sig!=SIGCHLD)stopping=1;
    if(wakefd[1]>=0)write(wakefd[1],&byte,1);
    errno=saved;
}

static void reap_children(void)
{
    int pid,i,status;
    while((pid=waitpid(-1,&status,WNOHANG))>0){
        for(i=0;i<16;i++)if(children[i]==pid)children[i]=0;
        if(trace)fprintf(stderr,"NXDSKTOP_CHILD_EXIT %d %d\n",pid,status);
    }
}

static int launch_app(char **args,int output)
{
    int slot,pid,i,fd;
    reap_children();
    for(slot=0;slot<16&&children[slot];slot++){}
    if(slot==16){fputs("nxdsktop: too many applications\n",stderr);return -1;}
    pid=fork();
    if(!pid){
        signal(SIGCHLD,SIG_DFL);signal(SIGTERM,SIG_DFL);
        signal(SIGINT,SIG_DFL);signal(SIGHUP,SIG_DFL);
        if(output>=0)dup2(output,1);
        fd=open("/dev/null",O_RDWR);
        if(fd>=0){dup2(fd,0);if(output<0)dup2(fd,1);}
        for(i=3;i<32;i++)close(i);
        execv(args[0],args);perror(args[0]);_exit(127);
    }
    if(pid<0){perror("nxdsktop: fork");return -1;}
    children[slot]=pid;
    if(trace)fprintf(stderr,"NXDSKTOP_LAUNCH %s %d\n",args[0],pid);
    return pid;
}

static void close_modal(void)
{
    if(modal_fd>=0){GrUnregisterInput(modal_fd);close(modal_fd);modal_fd=-1;}
}

static void open_modal(char **args,int kind)
{
    int fds[2];
    if(modal_fd>=0){GrBell();return;}
    if(pipe(fds)<0){perror("nxdsktop: pipe");return;}
    if(fcntl(fds[0],F_SETFL,O_NONBLOCK)<0||launch_app(args,fds[1])<0){close(fds[0]);close(fds[1]);return;}
    close(fds[1]);modal_fd=fds[0];modal_kind=kind;result_count=0;modal_bad=0;
    GrRegisterInput(modal_fd);
}

static void modal_input(void)
{
    char buf[64];int n,i,complete=0,kind;
    while((n=read(modal_fd,buf,sizeof(buf)))>0){
        for(i=0;i<n;i++){
            if(buf[i]=='\n'){complete=1;break;}
            if(buf[i]!='\r'){
                if(result_count+1<sizeof(result_line))result_line[result_count++]=buf[i];
                else modal_bad=1;
            }
        }
        if(complete)break;
    }
    if(!complete&&n<0&&(errno==EAGAIN||errno==EINTR))return;
    kind=modal_kind;close_modal();result_line[result_count]=0;
    if(trace)fprintf(stderr,"NXDSKTOP_RESULT %d %s\n",kind,
        complete&&!modal_bad?result_line:"[]");
    if(kind==1&&complete&&!modal_bad&&result_line[0]=='/'&&strcmp(result_line,"[]")){
        char *args[]={"/bin/nxterm","/bin/vi",result_line,NULL};
        launch_app(args,-1);
    }
}

static void draw_status(void)
{
    struct ripple_sysinfo info;
    time_t now=time(NULL);struct tm *tm=localtime(&now);
    char text[80];int fd;
    unsigned long free_kib=0,total_kib=0;
    fd=open("/dev/proc",O_RDONLY);
    if(fd>=0){
        if(!ioctl(fd,RIPPLE_SYSINFO,&info)&&info.version==RIPPLE_SYSINFO_VERSION){
            total_kib=info.ram_kib;free_kib=info.pool_kib-info.used_kib;
        }
        close(fd);
    }
    snprintf(text,sizeof(text),"%lu / %lu KiB free   %02d:%02d",free_kib,total_kib,
             tm?tm->tm_hour:0,tm?tm->tm_min:0);
    if(!strcmp(text,status_text))return;
    strcpy(status_text,text);win=panel;
    GrSetGCForeground(gc_bar,GrGetSysColor(GR_COLOR_BTNFACE));
    GrFillRect(win,gc_bar,START_WIDTH,0,width-START_WIDTH,TASKBAR_HEIGHT);
    GrSetGCForeground(gc_text,GrGetSysColor(GR_COLOR_WINDOWTEXT));
    GrText(win,gc_text,width-strlen(text)*8-8,TEXT_Y_OFFSET_TASKBAR,text,strlen(text),GR_TFASCII);
    if(trace)fprintf(stderr,"NXDSKTOP_STATUS %s\n",text);
}

static void paint_panel(void)
{
    win=panel;draw_taskbar();status_text[0]=0;draw_status();
}

static void paint_menu(void)
{
    win=popup;draw_menu();win=panel;
}

static void hide_menu(void)
{
    if(menu_open){menu_open=0;GrUnmapWindow(popup);}
}

static void show_message(char *title,char *text)
{
    char *args[]={"/bin/nxmsg","-ta","1",title,text,NULL};
    open_modal(args,0);
}

static void menu_action(int y)
{
    int item;
    char *single[2]={NULL,NULL};
    hide_menu();
    if(y<4||y>=MENU_HEIGHT)return;
    if(y<4+APP_COUNT*MENU_ITEM_HEIGHT)item=(y-4)/MENU_ITEM_HEIGHT;
    else if(y>=8+APP_COUNT*MENU_ITEM_HEIGHT)item=APP_COUNT+(y-8-APP_COUNT*MENU_ITEM_HEIGHT)/MENU_ITEM_EXIT_HEIGHT;
    else return;
    if(trace)fprintf(stderr,"NXDSKTOP_ACTION %d\n",item);
    switch(item){
    case MENU_ABOUT:show_message("About NXDSKTOP","NXDSKTOP 1.1\nBy Anton Andreev\nRiplix adaptation\nNano-X window system");break;
    case MENU_HELP:show_message("Desktop Help","Start launches apps.\nMove: drag the title bar.\nResize: bottom-right corner.\nLevee: Esc, :wq, Enter saves/quits.\nExit ends desktop. Shutdown saves disk.");break;
    case MENU_CALCULATOR:single[0]="/bin/nxcalc";break;
    case MENU_TERMINAL:single[0]="/bin/nxterm";break;
    case MENU_EDIT:{char *args[]={"/bin/nxselect",NULL};open_modal(args,1);break;}
    case MENU_PROCESSES:{char *args[]={"/bin/nxterm","/bin/top",NULL};launch_app(args,-1);break;}
    case MENU_CLOCK:single[0]="/bin/nxclock";break;
    case MENU_EYES:single[0]="/bin/nxeyes";break;
    case MENU_ROACHES:single[0]="/bin/nxroach";break;
    case MENU_TETRIS:single[0]="/bin/nxtetris";break;
    case MENU_IMAGES:single[0]="/bin/nxview";break;
    case MENU_KLOTSKI:single[0]="/bin/klotski";break;
    case MENU_SHIKAKU:single[0]="/bin/shikaku";break;
    case MENU_SYNC:sync();show_message("Sync disk","Files synchronized to disk.");break;
    case MENU_EXIT:stopping=1;break;
    case MENU_SHUTDOWN:uadmin(1,0,NULL);perror("nxdsktop: shutdown");break;
    }
    if(single[0])launch_app(single,-1);
}

static void desktop_cleanup(void)
{
    int i,n,live;
    if(cleaned)return;cleaned=1;
    close_modal();
    for(i=0;i<16;i++)if(children[i]>0){kill(children[i],SIGTERM);kill(children[i],SIGCONT);}
    for(n=0;n<50;n++){
        reap_children();live=0;for(i=0;i<16;i++)live|=children[i]>0;
        if(!live)break;usleep(10000);
    }
    for(i=0;i<16;i++)if(children[i]>0)kill(children[i],SIGKILL);
    while(waitpid(-1,NULL,0)>0||errno==EINTR){}
    n=wakefd[1];wakefd[1]=-1;
    if(wakefd[0]>=0){GrUnregisterInput(wakefd[0]);close(wakefd[0]);wakefd[0]=-1;}
    if(n>=0)close(n);
    if(connected){connected=0;GrClose();}
    if(trace)fprintf(stderr,"NXDSKTOP_EXIT\n");
}

int main(int argc,char **argv)
{
    GR_SCREEN_INFO screen;GR_EVENT e;char bytes[64];int i;
    if(argc>1){puts("usage: nxdsktop\nA lightweight Nano-X desktop. Use startx to start the complete session.");return !strcmp(argv[1],"--help")?0:2;}
    trace=getenv("NANOX_TRACE")!=NULL;
    if(GrOpen()<0){fputs("nxdsktop: cannot connect to Nano-X\n",stderr);return 1;}
    connected=1;atexit(desktop_cleanup);GrGetScreenInfo(&screen);width=screen.cols;
    if(pipe(wakefd)<0)return 1;
    for(i=0;i<2;i++)if(fcntl(wakefd[i],F_SETFL,O_NONBLOCK)<0)return 1;
    GrRegisterInput(wakefd[0]);
    signal(SIGCHLD,desktop_signal);signal(SIGTERM,desktop_signal);
    signal(SIGINT,desktop_signal);signal(SIGHUP,desktop_signal);
    panel=GrNewWindowEx(GR_WM_PROPS_NODECORATE|GR_WM_PROPS_NOAUTOMOVE|GR_WM_PROPS_NOFOCUS,
        "NXDSKTOP",GR_ROOT_WINDOW_ID,0,screen.rows-TASKBAR_HEIGHT,width,TASKBAR_HEIGHT,GrGetSysColor(GR_COLOR_BTNFACE));
    popup=GrNewWindowEx(GR_WM_PROPS_NODECORATE|GR_WM_PROPS_NOAUTOMOVE,
        "Start",GR_ROOT_WINDOW_ID,0,screen.rows-TASKBAR_HEIGHT-MENU_HEIGHT,MENU_WIDTH,MENU_HEIGHT,GrGetSysColor(GR_COLOR_BTNFACE));
    gc_bar=GrNewGC();gc_text=GrNewGC();
    if(!panel||!popup||!gc_bar||!gc_text)return 1;
    GrSetGCUseBackground(gc_text,GR_FALSE);
    GrSelectEvents(panel,GR_EVENT_MASK_EXPOSURE|GR_EVENT_MASK_BUTTON_DOWN|GR_EVENT_MASK_CLOSE_REQ|GR_EVENT_MASK_TIMER);
    GrSelectEvents(popup,GR_EVENT_MASK_EXPOSURE|GR_EVENT_MASK_BUTTON_DOWN|GR_EVENT_MASK_KEY_DOWN|GR_EVENT_MASK_FOCUS_OUT);
    GrMapWindow(panel);status_timer=GrCreateTimer(panel,1000);
    if(!status_timer)return 1;
    GrFlush();if(trace)fprintf(stderr,"NXDSKTOP_READY %d\n",getpid());
    while(!stopping){
        GrGetNextEvent(&e);
        if(e.type==GR_EVENT_TYPE_FDINPUT){
            if(e.fdinput.fd==wakefd[0]){while(read(wakefd[0],bytes,sizeof(bytes))>0){}reap_children();}
            else if(e.fdinput.fd==modal_fd)modal_input();
        }else if(e.type==GR_EVENT_TYPE_EXPOSURE){
            if(e.exposure.wid==panel)paint_panel();else if(e.exposure.wid==popup&&menu_open)paint_menu();
        }else if(e.type==GR_EVENT_TYPE_TIMER)draw_status();
        else if(e.type==GR_EVENT_TYPE_BUTTON_DOWN){
            if(!(e.button.changebuttons&GR_BUTTON_L))continue;
            if(e.button.wid==panel){
                if(e.button.x<START_WIDTH){
                    if(menu_open)hide_menu();else{menu_open=1;GrMapWindow(popup);GrRaiseWindow(popup);GrSetFocus(popup);}
                    if(trace)fprintf(stderr,"NXDSKTOP_MENU %d\n",menu_open);
                }else hide_menu();
            }else if(e.button.wid==popup&&menu_open)menu_action(e.button.y);
        }else if(e.type==GR_EVENT_TYPE_FOCUS_OUT||e.type==GR_EVENT_TYPE_KEY_DOWN)hide_menu();
        else if(e.type==GR_EVENT_TYPE_CLOSE_REQ)stopping=1;
        GrFlush();
    }
    return 0;
}
