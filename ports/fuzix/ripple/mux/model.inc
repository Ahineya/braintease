/* Screen ownership, split trees, PTY lifecycle and bounded history. */
static int new_node(void){int i;for(i=0;i<NODES;i++)if(!nodes[i].used){memset(&nodes[i],0,sizeof(nodes[i]));nodes[i].used=1;nodes[i].parent=-1;nodes[i].pane=-1;nodes[i].ratio=50;return i;}return -1;}
static void free_history(struct pane *p){unsigned i;if(p->history){for(i=0;i<p->limit;i++)free(p->history[i].cells);free(p->history);}p->history=NULL;}
static struct line *history_line(struct pane *p,unsigned i){return &p->history[(p->head+i)%p->limit];}
static struct vt_cell blank_cell={' ',7};
static struct vt_cell line_cell(struct pane *p,unsigned line,unsigned x){
    if(line<p->count){struct line *l=history_line(p,line);return x<l->width?l->cells[x]:blank_cell;}
    line-=p->count;
    return line<p->vt.rows&&x<p->vt.cols?vt_cells(&p->vt)[line*p->vt.cols+x]:blank_cell;
}
static void remember(struct vt *v,int amount){
    struct pane *p=v->user;unsigned row,at;struct line *l;struct vt_cell *next;
    if(amount>0&&!v->alternate&&!v->top&&v->bottom==v->rows-1&&p->limit){
        for(row=0;row<(unsigned)amount;row++){
            at=(p->head+p->count)%p->limit;l=&p->history[at];
            if(l->width!=v->cols){
                next=malloc((unsigned long)v->cols*sizeof(*next));
                if(!next)break;
                free(l->cells);l->cells=next;l->width=v->cols;
            }
            memcpy(l->cells,vt_cells(v)+row*v->cols,v->cols*sizeof(*l->cells));
            if(p->count<p->limit)p->count++;
            else {p->head=(p->head+1)%p->limit;if(mode==1&&p==focused()){
                if(copy_line)copy_line--;if(copy_top)copy_top--;if(copy_mark)copy_mark--;else mark_set=0;
            }}
        }
    }
    vt_dirty(v);
}
static int enqueue(struct pane *p,const char *s,unsigned n){
    if(n>QUEUE-p->queued)return -1;
    memcpy(p->input+p->queued,s,n);p->queued+=n;return 0;
}
static void answer(struct vt *v,const char *s){struct pane *p=v->user;if(enqueue(p,s,strlen(s)))notice("Terminal reply queue full");}
static void geometry_node(int n,unsigned x,unsigned y,unsigned w,unsigned h){
    struct node *q=&nodes[n];unsigned size;
    q->x=x;q->y=y;q->w=w;q->h=h;
    if(q->pane>=0){struct pane *p=&panes[q->pane];p->x=x;p->y=y;p->w=w;p->h=h;p->shown=1;return;}
    size=(q->vertical?w:h);size=(size-1)*q->ratio/100;
    if(q->vertical){
        if(size<MIN_COLS)size=MIN_COLS;if(w-1-size<MIN_COLS)size=w-1-MIN_COLS;
        geometry_node(q->a,x,y,size,h);geometry_node(q->b,x+size+1,y,w-size-1,h);
    }else{
        if(size<MIN_ROWS)size=MIN_ROWS;if(h-1-size<MIN_ROWS)size=h-1-MIN_ROWS;
        geometry_node(q->a,x,y,w,size);geometry_node(q->b,x,y+size+1,w,h-size-1);
    }
}
static unsigned min_width(int n){struct node *q=&nodes[n];unsigned a,b;if(q->pane>=0)return MIN_COLS;a=min_width(q->a);b=min_width(q->b);return q->vertical?a+b+1:(a>b?a:b);}
static unsigned min_height(int n){struct node *q=&nodes[n];unsigned a,b;if(q->pane>=0)return MIN_ROWS;a=min_height(q->a);b=min_height(q->b);return q->vertical?(a>b?a:b):a+b+1;}
static void layout(void){
    unsigned i;struct tab *t=&tabs[current];struct pane *p;struct winsize size;
    if(!need_layout)return;need_layout=0;
    for(i=0;i<PANES;i++)panes[i].shown=0;
    if(!t->used)return;
    if(t->zoom||min_width(t->root)>cols||min_height(t->root)>rows-2){
        p=focused();p->x=0;p->y=1;p->w=cols;p->h=rows-2;p->shown=1;
    }else geometry_node(t->root,0,1,cols,rows-2);
    for(i=0;i<PANES;i++)if(panes[i].used&&panes[i].shown){
        p=&panes[i];
        if(p->vt.cols!=p->w||p->vt.rows!=p->h){
            if(vt_resize(&p->vt,p->w,p->h)){notice("Not enough memory to resize pane");continue;}
            memset(&size,0,sizeof(size));size.ws_col=p->w;size.ws_row=p->h;
            ioctl(p->fd,TIOCSWINSZ,&size);
        }
    }
    dirty=1;full=1;
}
static int restart_command(char *out,char **argv){
    unsigned used=0,i,j;const char *s;
    if(!argv){out[0]=0;return 0;}
    for(i=0;argv[i];i++){
        if(used+3>=RESTART_SIZE)return -1;
        if(i)out[used++]=' ';out[used++]='\'';
        for(j=0;argv[i][j];j++){
            s=argv[i][j]=='\''?"'\\''":NULL;
            if(used+(s?4:1)+2>=RESTART_SIZE)return -1;
            if(s){memcpy(out+used,s,4);used+=4;}else out[used++]=argv[i][j];
        }
        out[used++]='\'';
    }
    out[used]=0;return 0;
}
static int create_pane(int tab,char **command,const char *directory,const char *restart){
    int i,j,n,fd,pid;char master[]="/dev/ptyp0",slave[]="/dev/ttyp0";
    struct pane *p;struct termios term;struct winsize size;char *shell[4];
    for(i=0;i<PANES&&panes[i].used;i++){}
    if(i==PANES){notice("Pane limit reached (12 per session)");return -1;}
    n=new_node();if(n<0){notice("Split tree is full");return -1;}
    p=&panes[i];memset(p,0,sizeof(*p));p->fd=-1;p->tab=tab;p->node=n;
    if(directory)snprintf(p->directory,sizeof(p->directory),"%s",directory);
    else if(!getcwd(p->directory,sizeof(p->directory))){nodes[n].used=0;notice("Cannot record pane startup directory");return -1;}
    if(restart)snprintf(p->restart,sizeof(p->restart),"%s",restart);
    else if(restart_command(p->restart,command)){nodes[n].used=0;notice("Startup command is too long to save");return -1;}
    if(vt_init_size(&p->vt,cols,rows-2)){nodes[n].used=0;notice("Not enough memory for a pane");return -1;}
    p->limit=options.history;
    if(p->limit){p->history=calloc(p->limit,sizeof(*p->history));if(!p->history){vt_destroy(&p->vt);nodes[n].used=0;notice("Not enough memory for scrollback");return -1;}}
    p->vt.user=p;p->vt.scroll=remember;p->vt.reply=answer;
    for(j=0;j<RIPPLE_PTY_COUNT;j++){
        master[9]=slave[9]=RIPPLE_PTY_DIGIT(j);fd=open(master,O_RDWR|O_NOCTTY|O_NONBLOCK);
        if(fd>=0)break;if(errno!=EBUSY)break;
    }
    if(fd<0){free_history(p);vt_destroy(&p->vt);nodes[n].used=0;notice("No free PTY or descriptor; close another terminal and retry");return -1;}
    if(!tcgetattr(fd,&term)){term.c_cc[VERASE]=127;tcsetattr(fd,TCSANOW,&term);}
    memset(&size,0,sizeof(size));size.ws_col=cols;size.ws_row=rows-2;ioctl(fd,TIOCSWINSZ,&size);
    pid=fork();
    if(pid<0){close(fd);free_history(p);vt_destroy(&p->vt);nodes[n].used=0;notice("Cannot start another process");return -1;}
    if(!pid){
        int tty;
        for(j=0;j<32;j++)close(j);
        if(setsid()<0)_exit(125);
        tty=open(slave,O_RDWR);if(tty<0)_exit(125);
        dup2(tty,0);dup2(tty,1);dup2(tty,2);if(tty>2)close(tty);
        signal(SIGHUP,SIG_DFL);signal(SIGTERM,SIG_DFL);signal(SIGINT,SIG_DFL);signal(SIGPIPE,SIG_DFL);signal(SIGCHLD,SIG_DFL);
        if(chdir(p->directory)){perror("mux: startup directory");_exit(126);}
        putenv("TERM=ansi");putenv("MUX=1");
        shell[0]=options.shell;shell[1]=NULL;
        if(restart&&*restart){shell[1]="-c";shell[2]=p->restart;shell[3]=NULL;}
        execvp(command?command[0]:shell[0],command?command:shell);
        perror("mux: exec");_exit(127);
    }
    p->fd=fd;p->pid=p->session=pid;p->used=1;nodes[n].pane=i;dirty=1;return i;
}
static int add_pane(int tab,char **command){
    const char *directory=tabs[tab].used?panes[tabs[tab].focus].directory:NULL;
    return create_pane(tab,command,directory,NULL);
}
static int new_tab(char **command){
    int i,p;for(i=0;i<TABS&&tabs[i].used;i++){}
    if(i==TABS){notice("Tab limit reached (8 per session)");return -1;}
    p=add_pane(i,command);if(p<0)return -1;
    memset(&tabs[i],0,sizeof(tabs[i]));tabs[i].used=1;tabs[i].root=panes[p].node;tabs[i].focus=p;
    snprintf(tabs[i].name,sizeof(tabs[i].name),"shell %d",i+1);
    current=i;need_layout=1;mode=help=closing=0;message[0]=0;return i;
}
static void split_pane(int vertical){
    struct tab *t=&tabs[current];struct pane *old=focused();int leaf=old->node,parent=nodes[leaf].parent,branch,p;
    if((vertical&&old->w<MIN_COLS*2+1)||(!vertical&&old->h<MIN_ROWS*2+1)){notice("Pane too small to split; zoom out or enlarge the terminal");return;}
    branch=new_node();if(branch<0)return;
    p=add_pane(current,NULL);if(p<0){nodes[branch].used=0;return;}
    nodes[branch].parent=parent;nodes[branch].a=leaf;nodes[branch].b=panes[p].node;nodes[branch].vertical=vertical;
    nodes[leaf].parent=branch;nodes[panes[p].node].parent=branch;
    if(parent<0)t->root=branch;else if(nodes[parent].a==leaf)nodes[parent].a=branch;else nodes[parent].b=branch;
    t->focus=p;t->zoom=0;need_layout=1;mode=0;
}
static void stop_pane(struct pane *p){
    int fd,group=0;unsigned slot;struct ripple_sysinfo sys;struct ripple_procinfo info;
    if(p->fd>=0)ioctl(p->fd,TIOCGPGRP,&group);
    if(p->session>0){
        /* Background jobs can have a different group from their pane shell.
         * Explicit pane/session closure must release every PTY reference,
         * including stopped jobs and programs ignoring the hangup signal. */
        fd=open("/dev/proc",O_RDONLY);
        if(fd>=0){
            if(!ioctl(fd,RIPPLE_SYSINFO,&sys))for(slot=0;slot<sys.slots;slot++){
                memset(&info,0,sizeof(info));info.slot=slot;
                if(!ioctl(fd,RIPPLE_PROCINFO,&info)&&info.pid&&info.session==(unsigned)p->session)kill(info.pid,SIGKILL);
            }
            close(fd);
        }
        if(group>1)kill(-group,SIGKILL);
        if(p->pid>0)kill(-p->pid,SIGKILL);
    }
    if(p->fd>=0)close(p->fd);p->fd=-1;
}
static void remove_pane(int index){
    struct pane *p=&panes[index];struct tab *t=&tabs[p->tab];int leaf=p->node,parent=nodes[leaf].parent,sibling,above,i;
    stop_pane(p);free_history(p);vt_destroy(&p->vt);p->used=0;nodes[leaf].used=0;
    if(parent<0){
        t->used=0;
        if(current==p->tab){for(i=1;i<=TABS;i++)if(tabs[(current+i)%TABS].used){current=(current+i)%TABS;break;}if(i>TABS)stopping=1;}
    }else{
        sibling=nodes[parent].a==leaf?nodes[parent].b:nodes[parent].a;above=nodes[parent].parent;nodes[sibling].parent=above;
        if(above<0)t->root=sibling;else if(nodes[above].a==parent)nodes[above].a=sibling;else nodes[above].b=sibling;
        nodes[parent].used=0;
        if(t->focus==index){for(i=0;i<PANES;i++)if(panes[i].used&&panes[i].tab==p->tab){t->focus=i;break;}}
    }
    mode=closing=0;need_layout=1;dirty=1;
}
static void break_pane(void){
    struct tab *old=&tabs[current],*next;struct pane *p=focused();
    int tab,index=old->focus,leaf=p->node,parent=nodes[leaf].parent,sibling,above,i;
    if(parent<0){notice("This pane already occupies its own tab");return;}
    for(tab=0;tab<TABS&&tabs[tab].used;tab++){}
    if(tab==TABS){notice("Tab limit reached (8 per session)");return;}
    sibling=nodes[parent].a==leaf?nodes[parent].b:nodes[parent].a;above=nodes[parent].parent;
    nodes[sibling].parent=above;
    if(above<0)old->root=sibling;
    else if(nodes[above].a==parent)nodes[above].a=sibling;else nodes[above].b=sibling;
    nodes[parent].used=0;nodes[leaf].parent=-1;old->zoom=0;
    for(i=0;i<PANES;i++)if(i!=index&&panes[i].used&&panes[i].tab==current){old->focus=i;break;}
    next=&tabs[tab];memset(next,0,sizeof(*next));next->used=1;next->root=leaf;next->focus=index;
    snprintf(next->name,sizeof(next->name),"pane %d",index+1);p->tab=tab;current=tab;
    need_layout=full=dirty=1;mode=help=closing=0;
    notice("Moved the running pane to its own tab");
}
static void cycle_tab(int step){int i,n;for(i=1;i<=TABS;i++){n=(current+TABS+step*i)%TABS;if(tabs[n].used){current=n;need_layout=1;mode=help=closing=0;message[0]=0;return;}}}
static void cycle_pane(void){int i,n;for(i=1;i<=PANES;i++){n=(tabs[current].focus+i)%PANES;if(panes[n].used&&panes[n].tab==current){tabs[current].focus=n;mode=0;need_layout=1;return;}}}
static void focus_direction(int dx,int dy){
    struct pane *a=focused(),*b;int i,best=-1;unsigned dist=65535,d;
    for(i=0;i<PANES;i++)if(panes[i].used&&panes[i].tab==current&&i!=tabs[current].focus){
        b=&panes[i];if(!b->shown)continue;
        if((dx<0&&b->x+b->w>a->x)||(dx>0&&b->x<a->x+a->w)||(dy<0&&b->y+b->h>a->y)||(dy>0&&b->y<a->y+a->h))continue;
        d=abs((int)(a->x*2+a->w)-(int)(b->x*2+b->w))+abs((int)(a->y*2+a->h)-(int)(b->y*2+b->h));
        if(d<dist){dist=d;best=i;}
    }
    if(best>=0){tabs[current].focus=best;mode=0;dirty=1;}
}
static void adjust_split(int amount){
    int n=nodes[focused()->node].parent;
    if(n<0){notice("This tab has no split to resize");return;}
    if(nodes[n].b==focused()->node)amount=-amount;
    if((int)nodes[n].ratio+amount>=10&&(int)nodes[n].ratio+amount<=90){nodes[n].ratio+=amount;need_layout=1;}
}
