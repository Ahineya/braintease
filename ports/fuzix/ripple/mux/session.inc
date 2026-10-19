/* A session owns PTYs independently of an attaching terminal. Local streams
 * carry framed input/resize requests and bounded raw ANSI output. */
static int control=-1;static char control_header[3];static unsigned control_used;
static int save_reply;static char control_reply[8];static unsigned control_sent;
static char rx_header[3],rx_body[256];static unsigned rx_head,rx_size,rx_used,rx_pos;
static void disconnect(void){
    if(client>=0)close(client);client=-1;out_start=out_count=0;
    rx_head=rx_size=rx_used=rx_pos=0;prefix_pending=escape_state=0;mode=help=closing=0;
    paste_count=0;full=dirty=1;rendering=0;
    save_reply=0;synchronized_output=0;
}
static void receive_request(void){
    int n;unsigned c;
    if(client<0||paste_count)return;
    if(rx_head<3){
        n=read(client,rx_header+rx_head,3-rx_head);
        if(n>0)rx_head+=n;else if(!n||(n<0&&!retry_error())){disconnect();return;}
        if(rx_head<3)return;
        rx_size=(unsigned char)rx_header[1]*256U+(unsigned char)rx_header[2];
        if(rx_size>sizeof(rx_body)||((rx_header[0]!='I'||!rx_size)&&(rx_header[0]!='R'||rx_size!=5)&&
            (rx_header[0]!='K'||rx_size)&&(rx_header[0]!='S'||rx_size))){disconnect();return;}
    }
    if(rx_used<rx_size){
        n=read(client,rx_body+rx_used,rx_size-rx_used);
        if(n>0)rx_used+=n;else if(!n||(n<0&&!retry_error())){disconnect();return;}
        if(rx_used<rx_size)return;
    }
    if(rx_header[0]=='R'){
        unsigned w=(unsigned char)rx_body[0]*256U+(unsigned char)rx_body[1];
        unsigned h=(unsigned char)rx_body[2]*256U+(unsigned char)rx_body[3];
        erase_character=(unsigned char)rx_body[4];
        if(!w||!h||screen_size(w,h))notice("Cannot allocate requested terminal size");
    }else if(rx_header[0]=='K')stopping=1;
    else if(rx_header[0]=='S'){
        int ok=!save_session();out_start=out_count=0;save_reply=1;
        emit(ok?"MUXSAVE1":"MUXSAVE0",8);
    }
    else while(rx_pos<rx_size&&client>=0&&!paste_count){
        if(focused()->queued>=QUEUE-64)break;
        c=(unsigned char)rx_body[rx_pos++];handle_input(c);
        if(need_layout)layout();
        if(stopping)break;
    }
    if(client>=0&&(rx_header[0]!='I'||rx_pos==rx_size))rx_head=rx_size=rx_used=rx_pos=0;
}
static void accept_client(void){
    int fd=accept(listenfd,NULL,NULL);if(fd<0)return;
    if(client>=0){
        if(control>=0){write(fd,"BUSY",4);close(fd);return;}
        if(nonblock(fd)<0){close(fd);return;}
        control=fd;control_used=0;write(fd,"MUXB",4);return;
    }
    if(nonblock(fd)<0){close(fd);return;}
    client=fd;out_start=out_count=0;emit("MUX1",4);full=dirty=need_layout=1;
    rx_head=rx_size=rx_used=rx_pos=0;
}
static void control_request(void){
    int n;if(control<0)return;
    if(control_used==3){
        n=write(control,control_reply+control_sent,8-control_sent);
        if(n>0){control_sent+=n;if(control_sent<8)return;}
        else if(n<0&&retry_error())return;
        close(control);control=-1;control_used=0;return;
    }
    n=read(control,control_header+control_used,3-control_used);
    if(n>0){
        control_used+=n;if(control_used<3)return;
        if(!control_header[1]&&!control_header[2]){
            if(control_header[0]=='K'){stopping=1;return;}
            if(control_header[0]=='S'){
                memcpy(control_reply,!save_session()?"MUXSAVE1":"MUXSAVE0",8);control_sent=0;return;
            }
        }
    }
    else if(n<0&&retry_error())return;
    close(control);control=-1;control_used=0;
}
static void reap(void){
    int pid,status,i;
    while((pid=waitpid(-1,&status,WNOHANG))>0)for(i=0;i<PANES;i++)if(panes[i].used&&panes[i].pid==pid){panes[i].pid=0;panes[i].dead=1;}
    child_changed=0;
}
static void server_cleanup(void){
    int i,status;unsigned tries;
    for(i=0;i<PANES;i++)if(panes[i].used)stop_pane(&panes[i]);
    for(tries=0;tries<20;tries++){
        int alive=0;for(i=0;i<PANES;i++)if(panes[i].used&&panes[i].pid>0){
            if(waitpid(panes[i].pid,&status,WNOHANG)==panes[i].pid)panes[i].pid=0;else alive=1;
        }
        if(!alive)break;usleep(10000);
    }
    for(i=0;i<PANES;i++)if(panes[i].used){
        if(panes[i].pid>0){kill(-panes[i].pid,SIGKILL);waitpid(panes[i].pid,&status,0);}
        free_history(&panes[i]);vt_destroy(&panes[i].vt);
    }
    free(screen);free(painted);
    if(listenfd>=0)close(listenfd);unlink(endpoint);
    if(client>=0)close(client);if(control>=0)close(control);
}
static int server(char **command,unsigned w,unsigned h,int ready){
    int fd,i,n,maxfd;char buffer[512];fd_set reads,writes;struct timeval timeout;
    char status=1;
    /* Release every inherited outer tty reference, including shell's safe fd. */
    for(i=0;i<32;i++)if(i!=listenfd&&i!=ready)close(i);
    if(setsid()<0){write(ready,&status,1);close(ready);server_cleanup();return 1;}
    fd=open("/dev/null",O_RDWR);if(fd>=0){dup2(fd,0);dup2(fd,1);dup2(fd,2);if(fd>2)close(fd);}
    signal(SIGHUP,SIG_IGN);signal(SIGPIPE,SIG_IGN);
    signal(SIGTERM,signal_stop);signal(SIGINT,signal_stop);signal(SIGCHLD,signal_child);
    if(screen_size(w,h)||(restoring?restore_layout(restoring):new_tab(command))<0){
        write(ready,&status,1);close(ready);free(restoring);restoring=NULL;server_cleanup();return 1;
    }
    free(restoring);restoring=NULL;status=0;write(ready,&status,1);close(ready);
    nonblock(listenfd);
    while(!stopping&&!terminated){
        if(child_changed)reap();
        control_request();
        update_clock();
        layout();paste_more();escape_timeout();
        for(i=0;i<PANES;i++)if(panes[i].used){
            struct pane *p=&panes[i];
            if(p->sync_since&&(long)(milliseconds()-p->sync_since)>=1000){p->vt.synchronized=0;p->sync_since=0;dirty=1;}
            if(p->queued){
                n=write(p->fd,p->input,p->queued);
                if(n>0){p->queued-=n;memmove(p->input,p->input+n,p->queued);}
                else if(n<0&&!retry_error())p->eof=1;
            }
            /* A bounded round per pane keeps continuous output fair. */
            n=read(p->fd,buffer,sizeof(buffer));
            if(n>0){
                vt_feed(&p->vt,buffer,n);
                if(!p->vt.synchronized)p->sync_since=0;
                else if(!p->sync_since){p->sync_since=milliseconds();if(!p->sync_since)p->sync_since=1;}
                if(p->tab!=current){if(!p->activity){p->activity=1;dirty=1;}}
                else if(p->shown)dirty=1;
            }else if(!n||(n<0&&!retry_error()))p->eof=1;
            if(p->eof||(p->dead&&n<=0)){remove_pane(i);if(stopping)break;}
        }
        if(stopping)break;
        /* Reap failures before a child managed to open its slave. */
        reap();layout();
        if(client>=0){
            if(!save_reply)receive_request();
            for(i=0;i<PANES;i++)if(panes[i].used&&panes[i].tab==current)panes[i].activity=0;
            if(!save_reply){if(full||dirty)compose();render();}
            flush_output();if(save_reply&&!out_count)disconnect();
        }
        FD_ZERO(&reads);FD_ZERO(&writes);maxfd=listenfd;FD_SET(listenfd,&reads);
        for(i=0;i<PANES;i++)if(panes[i].used){
            fd=panes[i].fd;FD_SET(fd,&reads);if(panes[i].queued)FD_SET(fd,&writes);if(fd>maxfd)maxfd=fd;
        }
        if(client>=0){
            if(!save_reply&&!paste_count&&focused()->queued<QUEUE-64)FD_SET(client,&reads);
            if(out_count)FD_SET(client,&writes);if(client>maxfd)maxfd=client;
        }
        if(control>=0){FD_SET(control,&reads);if(control_used==3)FD_SET(control,&writes);if(control>maxfd)maxfd=control;}
        /* Rendering and an already buffered request are runnable work. */
        timeout.tv_sec=escape_state?0:1;timeout.tv_usec=escape_state?50000:0;
        if(client>=0&&((rendering&&output_free()>100)||(rx_head==3&&rx_used==rx_size&&!paste_count&&focused()->queued<QUEUE-64))){timeout.tv_sec=0;timeout.tv_usec=0;}
        n=select(maxfd+1,&reads,&writes,NULL,&timeout);
        if(n<0){if(errno!=EINTR)break;continue;}
        if(FD_ISSET(listenfd,&reads))accept_client();
    }
    server_cleanup();return 0;
}
static struct termios outer_saved;static int outer_changed,stdout_flags=-1;
static void restore_terminal(void){
    if(stdout_flags>=0)fcntl(1,F_SETFL,stdout_flags);
    if(outer_changed){
        static const char end[]="\033[?2026l\033[0m\033(B\033[r\033[?7h\033[?25h\033[0 q\033[?1049l";
        unsigned at=0;int n;while(at<sizeof(end)-1){n=write(1,end+at,sizeof(end)-1-at);if(n>0)at+=n;else if(n<0&&errno==EINTR)continue;else break;}
        tcsetattr(0,TCSANOW,&outer_saved);outer_changed=0;
    }
}
static void dimensions(unsigned *w,unsigned *h){struct winsize size;*w=80;*h=24;if(!ioctl(0,TIOCGWINSZ,&size)){if(size.ws_col)*w=size.ws_col;if(size.ws_row)*h=size.ws_row;}}
static unsigned request(char *b,char type,const char *data,unsigned n){b[0]=type;b[1]=n/256;b[2]=n%256;if(n)memcpy(b+3,data,n);return n+3;}
static unsigned outer_width,outer_height;
static unsigned resize_request(char *b){unsigned w,h;char size[5];dimensions(&w,&h);outer_width=w;outer_height=h;size[0]=w/256;size[1]=w%256;size[2]=h/256;size[3]=h%256;size[4]=outer_saved.c_cc[VERASE];return request(b,'R',size,5);}
static int greet(int fd){
    unsigned at=0;int n;char greeting[4];fd_set set;struct timeval wait;
    while(at<4){FD_ZERO(&set);FD_SET(fd,&set);wait.tv_sec=5;wait.tv_usec=0;n=select(fd+1,&set,NULL,NULL,&wait);if(n<0&&errno==EINTR)continue;if(n<=0)return -1;n=read(fd,greeting+at,4-at);if(n<=0)return -1;at+=n;}
    if(!memcmp(greeting,"MUXB",4))return 2;
    if(!memcmp(greeting,"BUSY",4))return 1;
    return memcmp(greeting,"MUX1",4)?-1:0;
}
static int save_running(int fd){
    char frame[3]={'S',0,0},buffer[512],tail[8];
    unsigned sent=0,used=0,i;int n;fd_set set;struct timeval wait;
    unsigned long deadline=milliseconds()+10000;
    signal(SIGPIPE,SIG_IGN);
    n=greet(fd);if(n<0||n==1){close(fd);return 1;}
    while(sent<3){n=write(fd,frame+sent,3-sent);if(n>0)sent+=n;else if(n<0&&errno==EINTR)continue;else{close(fd);return 1;}}
    for(;;){
        if((long)(milliseconds()-deadline)>=0){close(fd);return 1;}
        FD_ZERO(&set);FD_SET(fd,&set);wait.tv_sec=1;wait.tv_usec=0;
        n=select(fd+1,&set,NULL,NULL,&wait);if(n<0&&errno==EINTR)continue;if(n<0){close(fd);return 1;}if(!n)continue;
        n=read(fd,buffer,sizeof(buffer));
        if(n<0&&errno==EINTR)continue;
        if(n<=0){close(fd);return n==0&&used==8&&!memcmp(tail,"MUXSAVE1",8)?0:1;}
        for(i=0;i<(unsigned)n;i++){if(used==8){memmove(tail,tail+1,7);used=7;}tail[used++]=buffer[i];}
    }
}
static int server_ready(int fd){
    fd_set set;struct timeval wait;char status;int n;
    do{FD_ZERO(&set);FD_SET(fd,&set);wait.tv_sec=30;wait.tv_usec=0;n=select(fd+1,&set,NULL,NULL,&wait);}while(n<0&&errno==EINTR);
    return n>0&&read(fd,&status,1)==1&&!status;
}
static int attach(int fd){
    struct termios raw;char sendbuf[QUEUE],display[2048],input[256];unsigned send_count,display_count=0,display_at=0;
    fd_set reads,writes;struct timeval wait;int n,maxfd=fd>1?fd:1,result=0;unsigned long size_poll=0;
    n=greet(fd);if(n){if(n>0)fputs("mux: session is already attached; detach it first (prefix d)\n",stderr);close(fd);return 1;}
    if(tcgetattr(0,&outer_saved)){perror("mux: terminal");close(fd);return 1;}
    raw=outer_saved;raw.c_lflag&=~(ICANON|ECHO|ISIG);raw.c_iflag&=~(ICRNL|INLCR|IXON);raw.c_oflag&=~OPOST;raw.c_cc[VMIN]=1;raw.c_cc[VTIME]=0;
    if(tcsetattr(0,TCSANOW,&raw)){close(fd);return 1;}
    outer_changed=1;atexit(restore_terminal);
    fputs("\033[?1049h\033[0m\033(B\033[r\033[?7l\033[?1l\033[?25l\033[H\033[2J",stdout);fflush(stdout);
    stdout_flags=fcntl(1,F_GETFL,0);nonblock(1);nonblock(fd);
    signal(SIGWINCH,signal_resize);signal(SIGHUP,signal_stop);signal(SIGTERM,signal_stop);signal(SIGINT,signal_stop);signal(SIGPIPE,SIG_IGN);
    send_count=resize_request(sendbuf);
    while(!terminated){
        /* The physical-console ABI discovers host size during TIOCGWINSZ.
         * PTYs notify through SIGWINCH; occasionally query for host consoles
         * as well, so this works without a new host signal/IRQ mechanism. */
        if(!size_poll||(long)(milliseconds()-size_poll)>=0){unsigned w,h;dimensions(&w,&h);if(w!=outer_width||h!=outer_height)resized=1;size_poll=milliseconds()+250;}
        if(resized&&send_count+8<=QUEUE){send_count+=resize_request(sendbuf+send_count);resized=0;}
        FD_ZERO(&reads);FD_ZERO(&writes);
        if(send_count+259<=QUEUE)FD_SET(0,&reads);
        if(!display_count)FD_SET(fd,&reads);
        if(send_count)FD_SET(fd,&writes);if(display_count)FD_SET(1,&writes);
        wait.tv_sec=0;wait.tv_usec=250000;n=select(maxfd+1,&reads,&writes,NULL,&wait);
        if(n<0){if(errno==EINTR)continue;result=1;break;}
        if(FD_ISSET(fd,&writes)&&send_count){n=write(fd,sendbuf,send_count);if(n>0){send_count-=n;memmove(sendbuf,sendbuf+n,send_count);}else if(n<0&&!retry_error())break;}
        if(FD_ISSET(1,&writes)&&display_count){n=write(1,display+display_at,display_count);if(n>0){display_at+=n;display_count-=n;}else if(n<0&&!retry_error()){result=1;break;}}
        if(FD_ISSET(fd,&reads)&&!display_count){n=read(fd,display,sizeof(display));if(n>0){display_count=n;display_at=0;}else if(!n||(n<0&&!retry_error()))break;}
        if(FD_ISSET(0,&reads)){n=read(0,input,sizeof(input));if(n>0)send_count+=request(sendbuf+send_count,'I',input,n);else if(!n||(n<0&&!retry_error()))break;}
    }
    close(fd);restore_terminal();printf("[mux: left session %s]\n",session_name);return result;
}
static void configuration(void){
    char path[160],line[160],*home=getenv("HOME"),*s,*value,*end;FILE *f;unsigned long n;
    if(!home)home="/";snprintf(path,sizeof(path),"%s/.muxrc",home);f=fopen(path,"r");if(!f)return;
    while(fgets(line,sizeof(line),f)){
        s=line;while(*s==' '||*s=='\t')s++;if(*s=='#'||!*s)continue;value=strchr(s,'=');if(!value)continue;*value++=0;
        end=s+strlen(s);while(end>s&&(end[-1]==' '||end[-1]=='\t'))*--end=0;
        while(*value==' '||*value=='\t')value++;end=value+strlen(value);while(end>value&&(end[-1]=='\n'||end[-1]=='\r'||end[-1]==' '||end[-1]=='\t'))*--end=0;
        if(!strcmp(s,"history")){n=strtoul(value,&end,10);if(*value&&!*end&&n<=1000)options.history=n;}
        else if(!strcmp(s,"prefix")){if(value[0]>='a'&&value[0]<='z'&&!value[1])options.prefix=value[0]-'a'+1;}
        else if(!strcmp(s,"shell")){if(*value=='/'&&strlen(value)<sizeof(options.shell))strcpy(options.shell,value);}
    }
    fclose(f);
}
static int session_path(const char *name,char *directory){
    struct stat st;unsigned i,len=strlen(name);
    if(!len||len>24){fputs("mux: session name must have 1..24 letters, digits, - or _\n",stderr);return -1;}
    for(i=0;i<len;i++)if(!((name[i]>='a'&&name[i]<='z')||(name[i]>='A'&&name[i]<='Z')||(name[i]>='0'&&name[i]<='9')||name[i]=='-'||name[i]=='_')){fputs("mux: invalid session name\n",stderr);return -1;}
    snprintf(directory,64,"/tmp/mux-%u",(unsigned)getuid());
    if(mkdir(directory,0700)<0&&errno!=EEXIST){perror("mux: session directory");return -1;}
    if(lstat(directory,&st)||!S_ISDIR(st.st_mode)||st.st_uid!=getuid()||(st.st_mode&077)){fputs("mux: session directory must be owned by you with mode 0700\n",stderr);return -1;}
    snprintf(endpoint,sizeof(endpoint),"%s/%s",directory,name);snprintf(session_name,sizeof(session_name),"%s",name);return 0;
}
static int connect_session(void){
    struct sockaddr_un address;int fd,error;memset(&address,0,sizeof(address));address.sun_family=AF_UNIX;strcpy(address.sun_path,endpoint);
    fd=socket(AF_UNIX,SOCK_STREAM,0);if(fd<0)return -1;
    if(connect(fd,(struct sockaddr *)&address,sizeof(address))){error=errno;close(fd);errno=error;return -1;}return fd;
}
static void usage(void){
    puts("usage: mux [COMMAND [ARG...]]\n"
         "       mux new NAME [COMMAND [ARG...]]\n"
         "       mux attach NAME | mux list | mux kill NAME\n"
         "       mux save [NAME] | mux restore [NAME] | mux saved\n"
         "Without arguments, start or attach session main. Default command: sh.\n"
         "Ctrl-B then ? opens help; ! move pane to tab, S save, C/D restart command/directory.\n"
         "Configure history, prefix and shell in ~/.muxrc. See man mux.");
}
int main(int argc,char **argv){
    char directory[384];const char *name="main";int action=0,first=1,fd,pid,ready[2],status;unsigned w,h;struct sockaddr_un address;struct stat st;char **command=NULL;
    if(argc>1&&(!strcmp(argv[1],"--help")||!strcmp(argv[1],"-h"))){usage();return 0;}
    if(argc>1&&(!strcmp(argv[1],"list")||!strcmp(argv[1],"-l")))action=3;
    else if(argc>1&&(!strcmp(argv[1],"attach")||!strcmp(argv[1],"-a")))action=2;
    else if(argc>1&&!strcmp(argv[1],"new"))action=1;
    else if(argc>1&&!strcmp(argv[1],"kill"))action=4;
    else if(argc>1&&!strcmp(argv[1],"save"))action=5;
    else if(argc>1&&!strcmp(argv[1],"restore"))action=6;
    else if(argc>1&&!strcmp(argv[1],"saved"))action=7;
    if(action==7){
        DIR *d;struct dirent *entry;char path[420];
        if(argc!=2){usage();return 2;}
        if(snapshot_directory(directory,0)){if(errno==ENOENT)return 0;perror("mux saved");return 1;}
        d=opendir(directory);if(!d)return 1;
        while((entry=readdir(d)))if(snapshot_name(entry->d_name)){
            snprintf(path,sizeof(path),"%s/%s",directory,entry->d_name);
            if(!lstat(path,&st)&&S_ISREG(st.st_mode)&&st.st_uid==getuid()&&!(st.st_mode&077))puts(entry->d_name);
        }
        closedir(d);return 0;
    }
    if(action&&action!=3){
        if(argc<3){if(action!=5&&action!=6){usage();return 2;}}
        else name=argv[2];first=3;
        if((action==5||action==6)&&argc>3){usage();return 2;}
    }
    if(session_path(name,directory))return 1;
    if(action==3){
        DIR *d=opendir(directory);struct dirent *entry;if(!d){perror("mux list");return 1;}
        while((entry=readdir(d)))if(entry->d_name[0]!='.')puts(entry->d_name);closedir(d);return 0;
    }
    if(action==0||action==1){if(first<argc&&!strcmp(argv[first],"--"))first++;if(first<argc)command=argv+first;}
    if(action!=4&&action!=5&&!isatty(0)){fputs("mux: an interactive terminal is required\n",stderr);return 1;}
    configuration();fd=connect_session();
    if(fd>=0){
        if(action==1||action==6||command){close(fd);fputs("mux: session already exists; attach it or choose a different name\n",stderr);return 1;}
        if(action==5){
            int failed=save_running(fd);
            if(failed)fputs("mux: could not save session (upgrade its running mux server if needed)\n",stderr);
            else printf("mux: saved %s (layout and restart commands)\n",session_name);
            return failed;
        }
        if(action==4){char k[3]={'K',0,0};int hello=greet(fd),got;unsigned sent=0;char discard[512];
            if(hello<0||hello==1){close(fd);return 1;}
            while(sent<3){got=write(fd,k+sent,3-sent);if(got>0)sent+=got;else if(got<0&&errno==EINTR)continue;else{close(fd);return 1;}}
            for(;;){fd_set set;struct timeval wait;FD_ZERO(&set);FD_SET(fd,&set);wait.tv_sec=10;wait.tv_usec=0;got=select(fd+1,&set,NULL,NULL,&wait);if(got<0&&errno==EINTR)continue;if(got<=0){fputs("mux: session shutdown timed out\n",stderr);close(fd);return 1;}got=read(fd,discard,sizeof(discard));if(!got)break;if(got<0&&errno!=EINTR){close(fd);return 1;}}
            close(fd);return 0;}
        return attach(fd);
    }
    if(action==2||action==4||action==5){perror("mux: session unavailable");return 1;}
    if(errno!=ENOENT&&errno!=ECONNREFUSED){perror("mux: connect");return 1;}
    if(!lstat(endpoint,&st)){
        if(!S_ISSOCK(st.st_mode)||st.st_uid!=getuid()){fputs("mux: refusing to replace a non-session path\n",stderr);return 1;}
        if(unlink(endpoint)){perror("mux: stale session");return 1;}
    }
    if(action==6){
        restoring=load_session();
        if(!restoring){fputs("mux: saved session is unavailable or invalid; check ~/.mux and restart directories\n",stderr);return 1;}
    }
    listenfd=socket(AF_UNIX,SOCK_STREAM,0);if(listenfd<0){perror("mux: socket");return 1;}
    memset(&address,0,sizeof(address));address.sun_family=AF_UNIX;strcpy(address.sun_path,endpoint);
    if(bind(listenfd,(struct sockaddr *)&address,sizeof(address))||listen(listenfd,4)){perror("mux: bind/listen");close(listenfd);return 1;}
    if(pipe(ready)){perror("mux: startup pipe");close(listenfd);unlink(endpoint);return 1;}
    dimensions(&w,&h);fflush(NULL);pid=fork();
    if(pid<0){perror("mux: fork");close(ready[0]);close(ready[1]);close(listenfd);unlink(endpoint);return 1;}
    if(!pid){close(ready[0]);_exit(server(command,w,h,ready[1]));}
    close(ready[1]);close(listenfd);listenfd=-1;free(restoring);restoring=NULL;
    if(!server_ready(ready[0])){
        close(ready[0]);kill(pid,SIGTERM);while(waitpid(pid,&status,0)<0&&errno==EINTR){}
        fputs("mux: could not start all panes; previous saved session kept\n",stderr);return 1;
    }
    close(ready[0]);fd=connect_session();
    if(fd<0){perror("mux: attach");return 1;}
    return attach(fd);
}
