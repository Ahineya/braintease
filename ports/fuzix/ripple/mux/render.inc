/* Retained cell composition and bounded ANSI output; no full-screen refresh per key. */
static unsigned char damaged[VT_MAX_ROWS];
static unsigned graphics_charset=2;
static time_t clock_minute=(time_t)-1;
static char header_clock[8]=" --:-- ";
static void update_clock(void){
    time_t now=time(NULL),minute;struct tm local;char text[8];
    if(now==(time_t)-1)return;
    minute=now/60;
    if(minute==clock_minute)return;
    clock_minute=minute;localtime_r(&now,&local);
    snprintf(text,sizeof(text)," %02d:%02d ",local.tm_hour,local.tm_min);
    if(strcmp(header_clock,text)){strcpy(header_clock,text);dirty=1;}
}
static const char *help_lines[]={
    "mux - tabs, tiled panes and persistent sessions",
    "Commands below follow Ctrl-B (configurable).",
    "c           New tab       n / p       Next / previous tab",
    "1 .. 8      Select tab    ,           Rename tab",
    "%           Split left/right",
    "\"           Split top/bottom",
    "Tab / o     Next pane     h j k l     Focus left/down/up/right",
    "+ / -       Resize split  z           Zoom / restore pane",
    "!           Move running pane to a new tab",
    "S           Save layout   C / D       Restart command / directory",
    "[           Scrollback    ]           Paste copied text",
    "d           Detach        x           Close pane (confirm)",
    "?           This help     r           Redraw",
    "Ctrl-B      Send prefix through to the application",
    "",
    "Scrollback: arrows or j/k, PgUp/PgDn or b/f; g/G top/bottom.",
    "Space starts line selection; y copies; / searches; n repeats.",
    "Esc or q returns to the application. Ctrl-C/Z reach its job.",
    "",
    "Detached sessions keep running until their last pane exits.",
    "mux save NAME | mux restore NAME | mux saved",
    "~/.muxrc: history=200, prefix=b, shell=/bin/sh",
    "Press any key to close help."
};
static void put(unsigned x,unsigned y,uint32_t ch,unsigned colour){
    unsigned at;if(x>=cols||y>=rows)return;at=y*cols+x;
    if(screen[at].ch!=ch||screen[at].colour!=colour){screen[at].ch=ch;screen[at].colour=colour;damaged[y]=1;}
}
static void label(unsigned x,unsigned y,const char *s,unsigned colour,unsigned limit){while(*s&&x<limit&&x<cols){unsigned c=(unsigned char)*s++;put(x++,y,c>=32&&c<127?c:'?',colour);}}
static void fill_row(unsigned y,unsigned colour){unsigned x;for(x=0;x<cols;x++)put(x,y,' ',colour);}
static int screen_size(unsigned width,unsigned height){
    struct vt_cell *a,*b;
    if(width<1)width=1;if(height<3)height=3;
    if(width>VT_MAX_COLS)width=VT_MAX_COLS;if(height>VT_MAX_ROWS)height=VT_MAX_ROWS;
    if(screen&&width==screen_cols&&height==screen_rows){cols=width;rows=height;return 0;}
    a=calloc((unsigned long)width*height,sizeof(*a));if(!a)return -1;
    b=malloc((unsigned long)width*height*sizeof(*b));if(!b){free(a);return -1;}
    free(screen);free(painted);screen=a;painted=b;cols=screen_cols=width;rows=screen_rows=height;
    need_layout=full=dirty=1;rendering=0;return 0;
}
static unsigned tab_label(unsigned i,char *text){
    unsigned activity=0,j;for(j=0;j<PANES;j++)if(panes[j].used&&panes[j].tab==(int)i&&panes[j].activity)activity=1;
    snprintf(text,48," %u:%s%s ",i+1,tabs[i].name,activity?"*":"");return strlen(text);
}
static void draw_header(void){
    unsigned x=0,i,start=0,total=0,width,colour,limit=cols>=20?cols-7:cols;char text[48];
    fill_row(0,0x17);
    for(i=0;i<=(unsigned)current;i++)if(tabs[i].used)total+=tab_label(i,text);
    while(total+(start?1:0)>limit&&start<(unsigned)current){if(tabs[start].used)total-=tab_label(start,text);start++;}
    if(start){put(x++,0,'<',0x1f);}
    for(i=start;i<TABS;i++)if(tabs[i].used){
        colour=(int)i==current?0x70:0x17;width=tab_label(i,text);
        label(x,0,text,colour,limit);x+=width;
        if(x>=limit){if(i<TABS-1)put(limit-1,0,'>',0x1f);break;}
    }
    if(limit<cols)label(limit,0,header_clock,0x1f,cols);
}
static void draw_footer(void){
    char text[240];struct pane *p=focused();
    fill_row(rows-1,0x17);
    if(mode==2)snprintf(text,sizeof(text),"Rename tab: %s_",prompt);
    else if(mode==3)snprintf(text,sizeof(text),"Search history: /%s_",prompt);
    else if(mode==4)snprintf(text,sizeof(text),"Restart command (empty=shell): %s_",prompt+ (prompt_len>100?prompt_len-100:0));
    else if(mode==5)snprintf(text,sizeof(text),"Restart directory: %s_",prompt+ (prompt_len>100?prompt_len-100:0));
    else if(closing)strcpy(text,"Close this pane and its running job? y / n");
    else if(mode==1)snprintf(text,sizeof(text)," HISTORY %u/%u %s  j/k PgUp/PgDn  Space select  y copy  / search  q return",copy_line+1,p->count+p->vt.rows,mark_set?"SELECT":"");
    else if(message[0])snprintf(text,sizeof(text)," %s",message);
    else snprintf(text,sizeof(text)," %s | pane %d | ^%c: ? help  c tab  %%/\" split  z zoom  d detach%s%s",session_name,tabs[current].focus+1,options.prefix+'@',tabs[current].zoom?" | ZOOM":"",prefix_pending?" | PREFIX":"");
    label(0,rows-1,text,0x17,cols);
}
static void compose(void){
    unsigned x,y,i,line,low,high;struct pane *p;
    if(!screen||!tabs[current].used)return;
    if(full){
        for(y=0;y<rows;y++)for(x=0;x<cols;x++){put(x,y,' ',7);painted[y*cols+x].ch=65535;}
        memset(damaged,1,rows);style=65535;graphics_charset=2;
        for(i=0;i<PANES;i++)if(panes[i].used&&panes[i].shown)vt_dirty(&panes[i].vt);
        /* Each split leaves a one-cell gutter; focus is indicated at its edge. */
        for(i=0;i<NODES;i++)if(nodes[i].used&&nodes[i].pane<0&&!tabs[current].zoom&&min_width(tabs[current].root)<=cols&&min_height(tabs[current].root)<=rows-2){
            int ancestor=i;while(nodes[ancestor].parent>=0)ancestor=nodes[ancestor].parent;
            if(ancestor!=tabs[current].root)continue;
            if(nodes[i].vertical){x=nodes[nodes[i].b].x-1;for(y=nodes[i].y;y<nodes[i].y+nodes[i].h;y++)put(x,y,'|',0x08);}
            else{y=nodes[nodes[i].b].y-1;for(x=nodes[i].x;x<nodes[i].x+nodes[i].w;x++)put(x,y,'-',0x08);}
        }
    }
    for(i=0;i<PANES;i++)if(panes[i].used&&panes[i].shown){
        p=&panes[i];
        /* Preserve the last composed pane until its application completes a
         * synchronized frame. Forwarding an intermediate erase as a complete
         * outer mux frame breaks both stable selection and atomic redraws. */
        if(p->vt.synchronized&&!full)continue;
        for(y=0;y<p->h;y++)if(full||p->vt.dirty[y]||(mode==1&&p==focused())||mode==3){
            line=p->count+y;
            if((mode==1||mode==3)&&p==focused())line=copy_top+y;
            low=copy_mark<copy_line?copy_mark:copy_line;high=copy_mark>copy_line?copy_mark:copy_line;
            for(x=0;x<p->w;x++){
                struct vt_cell c=line_cell(p,line,x);
                if((mode==1||mode==3)&&p==focused()&&((mark_set&&line>=low&&line<=high)||line==copy_line))c.colour=(c.colour&0xff00)|0x70;
                put(p->x+x,p->y+y,c.ch,c.colour);
            }
            p->vt.dirty[y]=0;
        }
    }
    if(full||dirty){draw_header();draw_footer();}
    if(help){
        unsigned h=sizeof(help_lines)/sizeof(help_lines[0]);
        for(y=1;y<rows-1;y++){
            fill_row(y,0x07);
            if(y-1<h)label(cols>76?2:0,y,help_lines[y-1],y==1?0x0e:0x07,cols);
        }
    }
    full=dirty=0;scan=0;rendering=1;
}
static unsigned output_free(void){return OUTPUT-out_count;}
static int emit(const char *s,unsigned n){
    if(n>output_free())return -1;
    if(out_start+out_count+n>OUTPUT){memmove(outgoing,outgoing+out_start,out_count);out_start=0;}
    memcpy(outgoing+out_start+out_count,s,n);out_count+=n;return 0;
}
static void flush_output(void){
    int n;if(client<0||!out_count)return;
    n=write(client,outgoing+out_start,out_count);
    if(n>0){out_start+=n;out_count-=n;if(!out_count)out_start=0;}
    else if(n<0&&!retry_error())disconnect();
}
static unsigned glyph(char *s,uint32_t c){
    unsigned at=0,i,j,n;unsigned char bytes[4];
    if(c==RU_CONTINUATION)return 0;
    if(graphics_charset!=0){s[0]=27;s[1]='(';s[2]='B';at=3;graphics_charset=0;}
    if(c<128){s[at]=c>=32&&c!=127?c:'?';return at+1;}
    for(i=0;i<ru_scalar_count(c);i++){
        n=ru_encode(ru_scalar(c,i),bytes);for(j=0;j<n;j++)s[at++]=bytes[j];
    }
    return at;
}
static void render(void){
    unsigned x,y,budget=8192,n,attr,fg,bg,run;char text[96];struct vt_cell c;struct pane *p;
    if(client<0||!screen||!rendering)return;
    if(!synchronized_output&&output_free()>100){emit("\033[?2026h",8);synchronized_output=1;}
    while(scan<cols*rows&&budget&&output_free()>100){
        y=scan/cols;
        if(!damaged[y]){scan=(y+1)*cols;continue;}
        x=scan%cols;c=screen[scan];
        if(c.ch==painted[scan].ch&&c.colour==painted[scan].colour){scan++;continue;}
        /* A run starts with one cursor move; adjacent changed cells reuse it. */
        n=snprintf(text,sizeof(text),"\033[%u;%uH",y+1,x+1);emit(text,n);
        while(scan<cols*rows&&scan/cols==y&&budget&&output_free()>100){
            c=screen[scan];if(c.ch==painted[scan].ch&&c.colour==painted[scan].colour)break;
            attr=c.colour;
            if(attr!=style){
                fg=attr&15;bg=(attr>>4)&15;
                /* Separate SGR attributes keep the colour encoding compact. */
                n=snprintf(text,sizeof(text),"\033[0;%u;%um",fg<8?30+fg:90+fg-8,bg<8?40+bg:100+bg-8);emit(text,n);
                if(attr&256)emit("\033[4m",4);if(attr&512)emit("\033[5m",4);style=attr;
            }
            if(c.ch==' '){
                for(run=1;scan+run<cols*rows&&(scan+run)/cols==y&&run<budget;run++){
                    struct vt_cell next=screen[scan+run];
                    if(next.ch!=' '||next.colour!=attr)break;
                }
                if(run>=8){
                    n=snprintf(text,sizeof(text),"\033[%uX",run);emit(text,n);
                    memcpy(painted+scan,screen+scan,run*sizeof(*screen));scan+=run;budget-=run;
                    /* ECH does not move the cursor. The next run positions it. */
                    break;
                }
            }
            n=glyph(text,c.ch);emit(text,n);painted[scan]=c;scan++;budget--;
        }
    }
    if(scan>=cols*rows&&output_free()>100){
        p=focused();x=p->x+p->vt.x;y=p->y+p->vt.y;
        n=snprintf(text,sizeof(text),"\033[%u;%uH\033[%u q\033[?25%c",y+1,x+1,p->vt.cursor_shape,(!help&&!mode&&p->vt.visible)?'h':'l');emit(text,n);
        emit("\033[?2026l",8);synchronized_output=0;
        memset(damaged,0,rows);rendering=0;
    }
}
