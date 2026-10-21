/* Prefix commands leave ordinary input, including Ctrl-C/Z and Tab, to the PTY. */
static unsigned escape_state,escape_param;
static unsigned long escape_deadline;
static void mouse_timeout(void);
static void cancel_mouse_capture(void);
static unsigned long milliseconds(void){struct timespec t;if(clock_gettime(CLOCK_MONOTONIC,&t))return 0;return t.tv_sec*1000UL+t.tv_nsec/1000000UL;}
static void copy_key(unsigned c);
static void escape_timeout(void){mouse_timeout();if(escape_state&&(long)(milliseconds()-escape_deadline)>=0){escape_state=0;if(mode==1)copy_key(27);else{if(mode==6||mode==7)mode=0;prefix_pending=0;dirty=1;}}}
static void copy_reveal(void){
    struct pane *p=focused();unsigned total=p->count+p->vt.rows;
    if(copy_line>=total)copy_line=total-1;
    if(copy_line<copy_top)copy_top=copy_line;
    if(copy_line>=copy_top+p->h)copy_top=copy_line-p->h+1;
    if(copy_top>total-p->h)copy_top=total-p->h;
    full=dirty=1;
}
static void copy_move(int amount){
    if(amount<0)copy_line=(unsigned)(-amount)>copy_line?0:copy_line+amount;
    else{unsigned end=focused()->count+focused()->vt.rows-1;copy_line=(unsigned)amount>end-copy_line?end:copy_line+amount;}
    copy_reveal();
}
static void copy_enter(void){
    struct pane *p=focused();mode=1;mark_set=0;copy_line=p->count+p->vt.rows-1;copy_top=p->count;
    copy_move(-(int)(p->h/2));
}
static int line_matches(struct pane *p,unsigned line,const char *s){
    unsigned x,j,n=strlen(s);if(!n||n>p->vt.cols)return 0;
    for(x=0;x+n<=p->vt.cols;x++){for(j=0;j<n;j++)if(line_cell(p,line,x+j).ch!=(unsigned char)s[j])break;if(j==n)return 1;}
    return 0;
}
static void find_history(void){
    struct pane *p=focused();unsigned total=p->count+p->vt.rows,i,line;
    if(!search[0])return;
    for(i=1;i<=total;i++){line=(copy_line+total-i)%total;if(line_matches(p,line,search)){copy_line=line;copy_reveal();return;}}
    notice("No matching line in retained history");
}
static void copy_selection(void){
    struct pane *p=focused();unsigned line,x,end,first=copy_line,last=copy_line;
    if(mark_set){first=copy_mark<copy_line?copy_mark:copy_line;last=copy_mark>copy_line?copy_mark:copy_line;}
    copied=0;
    for(line=first;line<=last;line++){
        end=p->vt.cols;while(end&&line_cell(p,line,end-1).ch==' ')end--;
        for(x=0;x<end;x++){
            uint32_t c=line_cell(p,line,x).ch;unsigned i,j,n;unsigned char bytes[4];
            if(c==RU_CONTINUATION)continue;
            for(i=0;i<ru_scalar_count(c);i++){
                n=ru_encode(ru_scalar(c,i),bytes);
                if(copied+n+2>=COPY_SIZE){notice("Selection exceeds 8191 octets; select fewer lines");copied=0;return;}
                for(j=0;j<n;j++)clipboard[copied++]=bytes[j];
            }
        }
        if(line<last)clipboard[copied++]='\n';
    }
    clipboard[copied]=0;mode=0;mark_set=0;full=1;notice("Copied selection; prefix ] pastes it");
}
static unsigned paste_at,paste_count;
static void paste_more(void){
    struct pane *p=focused();unsigned n;
    if(!paste_count)return;
    if(tabs[current].floating&&p->minimized){paste_count=0;return;}
    n=QUEUE-p->queued;if(n>paste_count)n=paste_count;
    enqueue(p,clipboard+paste_at,n);paste_at+=n;paste_count-=n;
}
static void run_program(void){
    char recipe[RESTART_SIZE],shell[RESTART_SIZE],title[RESTART_SIZE],name[RESTART_SIZE];
    char *args[2]={options.shell,NULL};const char *directory=pane_directory(focused()),*at=prompt;
    unsigned n=0,quote=0;int result,p;const char *word;
    while(*at==' '||*at=='\t')at++;
    if(!*at){notice("Enter a program name or command");return;}
    if(restart_command(shell,args)||strlen(at)+strlen(shell)+9>=sizeof(recipe)){
        notice("Run command is too long");return;
    }
    snprintf(recipe,sizeof(recipe),"%s; exec %s",at,shell);
    word=at;
    while(*word&&n+1<sizeof(name)){
        unsigned c=(unsigned char)*word++;
        if(!quote&&(c==' '||c=='\t'))break;
        if((c=='\''||c=='"')&&(!quote||quote==c)){quote=quote?0:c;continue;}
        if(c=='\\'&&quote!='\''&&*word)c=(unsigned char)*word++;
        name[n++]=c;
    }
    name[n]=0;
    {char *title_args[2]={name,NULL};if(restart_command(title,title_args))title[0]=0;}
    if(tabs[current].floating){p=floating_recipe(directory,recipe);if(p<0)return;}
    else {result=new_tab_recipe(NULL,directory,recipe);if(result<0)return;p=tabs[result].focus;}
    program_title(&panes[p],title);
    if(!tabs[current].floating)snprintf(tabs[current].name,sizeof(tabs[current].name),"%s",panes[p].title);
    mode=0;full=dirty=1;
}
static void prompt_input(unsigned c){
    if(mode==9&&c!=13&&c!=10)message[0]=0;
    struct stat st;unsigned limit=mode==8?sizeof(focused()->title)-1:mode==2?24:mode==3?sizeof(search)-1:mode==5?DIRECTORY_SIZE-1:RESTART_SIZE-1;
    if(c==27||c==3){mode=mode==3?1:0;prompt_len=0;full=dirty=1;return;}
    if(c=='\r'||c=='\n'){
        if(mode==9){run_program();return;}
        if(mode==2){if(prompt_len)snprintf(tabs[current].name,sizeof(tabs[current].name),"%s",prompt);mode=0;}
        else if(mode==8){if(prompt_len){snprintf(focused()->title,sizeof(focused()->title),"%s",prompt);focused()->title_override=1;}else{focused()->title_override=0;program_title(focused(),focused()->auto_valid?focused()->auto_command:"");}mode=0;}
        else if(mode==3){strcpy(search,prompt);mode=1;find_history();}
        else if(mode==4){strcpy(focused()->restart,prompt);focused()->restart_override=1;mode=0;notice("Restart command updated; prefix S saves the session");}
        else if(mode==5){
            mode=0;
            if(prompt[0]!='/'||stat(prompt,&st)||!S_ISDIR(st.st_mode))notice("Restart directory must be an existing absolute directory");
            else{strcpy(focused()->directory,prompt);focused()->directory_override=1;notice("Restart directory updated; prefix S saves the session");}
        }
        full=dirty=1;return;
    }
    if(c==21){prompt_len=0;prompt[0]=0;}
    else if(c==127||c==8){if(prompt_len){prompt_len=ru_previous_grapheme((const unsigned char *)prompt,prompt_len,prompt_len);prompt[prompt_len]=0;}}
    else if(c>=32&&c!=127&&c<256&&prompt_len<limit){prompt[prompt_len++]=c;prompt[prompt_len]=0;}
    dirty=1;
}
static void copy_key(unsigned c){
    struct pane *p=focused();
    if(c=='q'||c==27||c==3){mode=0;mark_set=0;full=dirty=1;}
    else if(c=='k')copy_move(-1);else if(c=='j')copy_move(1);
    else if(c=='b')copy_move(-(int)(p->h>1?p->h-1:1));
    else if(c=='f')copy_move(p->h>1?p->h-1:1);
    else if(c=='g'){copy_line=0;copy_reveal();}
    else if(c=='G'){copy_line=p->count+p->vt.rows-1;copy_reveal();}
    else if(c==' '){mark_set=!mark_set;copy_mark=copy_line;full=dirty=1;}
    else if(c=='y')copy_selection();
    else if(c=='/'){mode=3;prompt_len=0;prompt[0]=0;dirty=1;}
    else if(c=='n')find_history();
}
static void command_key(unsigned c){
    cancel_mouse_capture();
    prefix_pending=0;dirty=1;message[0]=0;
    if(c==options.prefix){char b=c;if(!tabs[current].floating||!focused()->minimized)enqueue(focused(),&b,1);}
    else if(c=='c')new_tab(NULL);
    else if(c=='f')new_desktop(NULL);
    else if(c=='w')floating_window();
    else if(c=='e'){mode=9;prompt_len=0;prompt[0]=0;full=1;}
    else if(c=='M')minimize_pane();
    else if(c=='m'||c=='v'||c=='t'){
        if(!tabs[current].floating)notice("Window commands need a desktop tab (prefix f)");
        else if(focused()->minimized)notice("Restore a window from the task bar first");
        else if(c=='t'){mode=8;snprintf(prompt,sizeof(prompt),"%s",focused()->title);prompt_len=strlen(prompt);}
        else {tabs[current].zoom=0;need_layout=1;mode=c=='m'?6:7;}
    }
    else if(c=='n')cycle_tab(1);else if(c=='p')cycle_tab(-1);
    else if(c>='1'&&c<='8'){if(tabs[c-'1'].used){current=c-'1';need_layout=1;mode=0;}else notice("That tab is not open");}
    else if(c=='%')split_pane(1);else if(c=='"')split_pane(0);
    else if(c=='o'||c==9)cycle_pane();
    else if(c=='h')focus_direction(-1,0);else if(c=='l')focus_direction(1,0);
    else if(c=='k')focus_direction(0,-1);else if(c=='j')focus_direction(0,1);
    else if(c=='+')adjust_split(5);else if(c=='-')adjust_split(-5);
    else if(c=='z'){if(tabs[current].floating)raise_pane(tabs[current].focus);tabs[current].zoom=!tabs[current].zoom;need_layout=1;}
    else if(c=='!')break_pane();
    else if(c=='S'){if(save_session())notice("Cannot save session in ~/.mux");}
    else if(c=='A'){focused()->restart_override=focused()->directory_override=0;notice("Automatic foreground command and directory restored; prefix S saves");}
    else if(c=='C'){mode=4;strcpy(prompt,focused()->restart);prompt_len=strlen(prompt);dirty=1;}
    else if(c=='D'){mode=5;strcpy(prompt,focused()->directory);prompt_len=strlen(prompt);dirty=1;}
    else if(c==','){mode=2;snprintf(prompt,sizeof(prompt),"%s",tabs[current].name);prompt_len=strlen(prompt);dirty=1;}
    else if(c=='[')copy_enter();
    else if(c==']'){paste_at=0;paste_count=copied;paste_more();if(!copied)notice("Nothing copied yet");}
    else if(c=='?'){help=1;full=1;}
    else if(c=='r'||c==12)full=1;
    else if(c=='x')closing=1;
    else if(c=='d')disconnect();
    else notice("Unknown mux command; prefix ? shows help");
}
static void floating_key(unsigned c){
    struct pane *p=focused();int dx=0,dy=0;
    if(c==27||c==3||c==13||c==10){mode=0;dirty=1;return;}
    if(c=='h')dx=-1;else if(c=='l')dx=1;else if(c=='k')dy=-1;else if(c=='j')dy=1;else return;
    if(!p->framed)return;
    if(mode==6)floating_set(p,(int)p->x-1+dx,(int)p->y-1+dy,p->w+2,p->h+2);
    else floating_set(p,p->x-1,p->y-1,(int)p->w+2+dx,(int)p->h+2+dy);
    dirty=1;
}
static void decoded_arrow(unsigned c){
    if(mode==6||mode==7){if(c=='A')floating_key('k');else if(c=='B')floating_key('j');else if(c=='C')floating_key('l');else if(c=='D')floating_key('h');return;}
    if(mode==1){if(c=='A')copy_key('k');else if(c=='B')copy_key('j');else if(c=='H')copy_key('g');else if(c=='F')copy_key('G');}
    else if(prefix_pending){if(c=='A')command_key('k');else if(c=='B')command_key('j');else if(c=='C')command_key('l');else if(c=='D')command_key('h');}
}
static void handle_key(unsigned c){
    char b=c;
    if(help){help=0;full=dirty=1;return;}
    if(closing){closing=0;if(c=='y'||c=='Y')remove_pane(tabs[current].focus);else dirty=1;return;}
    if(mode>=2&&mode!=6&&mode!=7){prompt_input(c);return;}
    /* Only interpret escape keys in mux modes. App input otherwise stays raw. */
    if((mode==1||mode==6||mode==7||prefix_pending)&&escape_state){
        if(escape_state==1){if(c=='['||c=='O'){escape_state=2;escape_param=0;return;}escape_state=0;if(mode==6||mode==7)floating_key(27);else copy_key(27);return;}
        if(c>='0'&&c<='9'){escape_param=escape_param*10+c-'0';if(escape_param>999)escape_state=0;return;}
        escape_state=0;
        if(c=='~'){if(mode==1){if(escape_param==5)copy_key('b');else if(escape_param==6)copy_key('f');}}
        else decoded_arrow(c);return;
    }
    if((mode==1||mode==6||mode==7||prefix_pending)&&c==27){escape_state=1;escape_deadline=milliseconds()+100;return;}
    if(mode==6||mode==7){floating_key(c);return;}
    if(prefix_pending){command_key(c);return;}
    if(mode==1){copy_key(c);return;}
    if(c==options.prefix){prefix_pending=1;dirty=1;return;}
    if(message[0]){message[0]=0;dirty=1;}
    /* Console input may encode Backspace as ^H, while GUI/Telnet use DEL.
     * Panes consistently advertise DEL; translate the attaching tty erase key. */
    if(c==erase_character&&(c==8||c==127))b=127;
    if(tabs[current].floating&&focused()->minimized)return;
    if(enqueue(focused(),&b,1))notice("Pane input queue is full; waiting for the application");
}

/* Outer coordinates belong to mux; each pane's VT owns its mouse modes. */
static int mouse_pane=-1,mouse_pid,mouse_split=-1,mouse_tab=-1,mouse_decoration;
static int mouse_window=-1,window_pid,window_edges,window_mx,window_my,window_x,window_y,window_w,window_h;
static char mouse_scan[40];static unsigned mouse_used,mouse_paste;
static unsigned long mouse_deadline;
static void cancel_mouse_capture(void) {
    unsigned i,j;char text[40];
    for(i=0;i<PANES;i++)if(panes[i].used) {
        struct vt *v=&panes[i].vt;
        for(j=0;j<3;j++)if(v->mouse_buttons&(1U<<j)) {
            unsigned n=vt_mouse_report(v,text,j,v->mouse_x,v->mouse_y,1,0,0);
            if(n)enqueue(&panes[i],text,n);
        }
        v->mouse_buttons=v->mouse_known=0;
    }
    mouse_pane=mouse_split=mouse_tab=mouse_window=-1;mouse_decoration=0;
}
static void mouse_route(unsigned code,int x,int y,int released) {
    unsigned i,button=code&3,motion=code&32;int hit=-1;char text[40];struct pane *p;
    if(mouse_decoration){if(released&&button==0)mouse_decoration=0;return;}
    if(mode==9){
        unsigned left,top,width;
        if(!motion&&!released&&button==0&&!(code&64)&&run_box(&left,&top,&width)&&y==(int)top+5){
            if(x>=(int)left+2&&x<(int)left+9){mouse_decoration=1;prompt_input(13);}
            else if(x>=(int)left+11&&x<(int)left+21){mouse_decoration=1;prompt_input(27);}
        }
        return;
    }
    if(mouse_tab>=0&&mouse_tab!=current)cancel_mouse_capture();
    if(mouse_window>=0){
        struct pane *w=&panes[mouse_window];
        if(!w->used||w->pid!=window_pid||!w->shown||!w->framed){cancel_mouse_capture();return;}
        if(motion&&!released){
            int dx=x-window_mx,dy=y-window_my,left=window_x,top=window_y,right=left+window_w,bottom=top+window_h;
            if(!window_edges)floating_set(w,left+dx,top+dy,window_w,window_h);
            else {
                if(window_edges&1){left+=dx;if(left<0)left=0;if(left>right-MIN_COLS-2)left=right-MIN_COLS-2;}
                if(window_edges&2){right+=dx;if(right>(int)cols)right=cols;if(right<left+MIN_COLS+2)right=left+MIN_COLS+2;}
                if(window_edges&4){top+=dy;if(top<1)top=1;if(top>bottom-MIN_ROWS-2)top=bottom-MIN_ROWS-2;}
                if(window_edges&8){bottom+=dy;if(bottom>(int)rows-2)bottom=rows-2;if(bottom<top+MIN_ROWS+2)bottom=top+MIN_ROWS+2;}
                floating_set(w,left,top,right-left,bottom-top);
            }
        }
        if(released&&button==0)cancel_mouse_capture();return;
    }
    if(mouse_split>=0) {
        struct node *q=&nodes[mouse_split];
        if(!q->used){cancel_mouse_capture();return;}
        if(motion&&!released) {
            int size=q->vertical?q->w:q->h,at=(q->vertical?x-(int)q->x:y-(int)q->y);
            unsigned low=q->vertical?min_width(q->a):min_height(q->a),high=q->vertical?min_width(q->b):min_height(q->b);
            if(size>1&&(unsigned)(size-1)>=low+high) {
                if(at<(int)low)at=low;if(at>size-1-(int)high)at=size-1-high;
                q->ratio=((unsigned long)at*100+size-2)/(size-1);need_layout=1;
            }
        }
        if(released)cancel_mouse_capture();return;
    }
    if(mouse_pane>=0) {
        if(!panes[mouse_pane].used||panes[mouse_pane].pid!=mouse_pid||!panes[mouse_pane].shown){cancel_mouse_capture();return;}
        hit=mouse_pane;
    } else if(!released&&!motion&&button==0&&!(code&64)) {
        if(y==0) {
            unsigned start=0,total=0,at=0,limit=cols>=20?cols-7:cols;char label[48];
            for(i=0;i<=(unsigned)current;i++)if(tabs[i].used)total+=tab_label(i,label);
            while(total+(start?1:0)>limit&&start<(unsigned)current){if(tabs[start].used)total-=tab_label(start,label);start++;}
            if(start)at++;
            for(i=start;i<TABS&&at<limit;i++)if(tabs[i].used){unsigned end=at+tab_label(i,label);if(x>=(int)at&&x<(int)end&&x<(int)limit){current=i;need_layout=1;mode=help=closing=0;break;}at=end;}
            return;
        }
        if(!help&&!mode&&!closing&&!tabs[current].floating&&!tabs[current].zoom)for(i=0;i<NODES;i++)if(nodes[i].used&&nodes[i].pane<0) {
            struct node *q=&nodes[i],*a=&nodes[q->a];
            if(x>=(int)q->x&&y>=(int)q->y&&x<(int)(q->x+q->w)&&y<(int)(q->y+q->h)&&
               (q->vertical?x==(int)(a->x+a->w):y==(int)(a->y+a->h))) {
                /* Reject stale nodes from hidden tabs. */
                int root=i;while(nodes[root].parent>=0)root=nodes[root].parent;
                if(root==tabs[current].root){mouse_split=i;mouse_tab=current;return;}
            }
        }
    }
    if(hit<0&&tabs[current].floating){
        if(!help&&!mode&&!closing&&cols>=MIN_COLS+2&&rows>=MIN_ROWS+5&&y==(int)rows-2){
            if(!motion&&!released&&button==0&&!(code&64)){int target=desktop_task(x);if(target>=0)raise_pane(target);}return;
        }
        hit=floating_hit(x,y);
        if(hit<0)return;
        p=&panes[hit];
        if(p->framed&&(x<(int)p->x||x>=(int)(p->x+p->w)||y<(int)p->y||y>=(int)(p->y+p->h))){
            if(help||mode||closing)return;
            if(!motion&&!released&&button==0&&!(code&64)){
                raise_pane(hit);
                if(y==(int)p->y-1&&x>=(int)(p->x+p->w)-9&&x<(int)(p->x+p->w)){
                    mouse_decoration=1;
                    /* A title-bar close is already an explicit mouse action. Keep
                     * keyboard confirmation on prefix x, without trapping the
                     * desktop in a keyboard-only modal after clicking [x]. */
                    if(x>=(int)(p->x+p->w)-3)remove_pane(hit);
                    else if(x>=(int)(p->x+p->w)-6){tabs[current].zoom=!tabs[current].zoom;need_layout=1;}
                    else minimize_pane();
                }else if(!tabs[current].zoom){
                    mouse_window=hit;window_pid=p->pid;mouse_tab=current;
                    window_mx=x;window_my=y;window_x=p->x-1;window_y=p->y-1;window_w=p->w+2;window_h=p->h+2;
                    window_edges=(x==(int)p->x-1?1:0)|(x==(int)(p->x+p->w)?2:0)|(y==(int)(p->y+p->h)?8:0);
                    if(y==(int)p->y-1&&window_edges)window_edges|=4;
                }
            }
            return;
        }
    }
    if(hit<0)for(i=0;i<PANES;i++)if(panes[i].used&&panes[i].shown&&x>=(int)panes[i].x&&y>=(int)panes[i].y&&x<(int)(panes[i].x+panes[i].w)&&y<(int)(panes[i].y+panes[i].h)){hit=i;break;}
    if(hit<0)return;
    if(!help&&!mode&&!closing&&!motion&&!released&&button==0&&!(code&64)&&tabs[current].focus!=hit){raise_pane(hit);}
    if(help||closing||mode>=2)return;
    if(mode==1){if(code&64){copy_move(button==0?-3:3);return;}return;}
    p=&panes[hit];
    if(!p->vt.mouse_mode)return;
    if(motion&&p->queued>QUEUE-128)return;
    i=vt_mouse_report(&p->vt,text,code&67,x-(int)p->x,y-(int)p->y,released,!!motion,code&28);
    if(i&&enqueue(p,text,i))notice("Pane mouse input queue is full");
    if(!motion&&!released&&button<3&&!(code&64)){mouse_pane=hit;mouse_pid=p->pid;mouse_tab=current;}
    if(released&&!p->vt.mouse_buttons)mouse_pane=mouse_tab=-1;
}
static void pasted_key(unsigned c){
    char b=c;
    if(mode==9){if(c>=32&&c!=127)prompt_input(c);}
    else if(!tabs[current].floating||!focused()->minimized)enqueue(focused(),&b,1);
}
static void mouse_flush(void) {
    unsigned i,n=mouse_used;mouse_used=0;
    for(i=0;i<n;i++) {
        if(mouse_paste)pasted_key((unsigned char)mouse_scan[i]);
        else handle_key((unsigned char)mouse_scan[i]);
    }
}
static void mouse_timeout(void) {if(mouse_used&&(long)(milliseconds()-mouse_deadline)>=0)mouse_flush();}
static void handle_input(unsigned c) {
    unsigned values[3]={0,0,0},part=0,i;int digit=0;
    if(!mouse_used&&c!=27){if(mouse_paste)pasted_key(c);else handle_key(c);return;}
    if(mouse_used==sizeof(mouse_scan)-1)mouse_flush();
    if(!mouse_used)mouse_deadline=milliseconds()+150;
    mouse_scan[mouse_used++]=c;
    if(mouse_used==1)return;
    if(mouse_used==2){if(c!='[')mouse_flush();return;}
    if(c<'@'||c>'~')return;
    mouse_scan[mouse_used]=0;
    if(!strcmp(mouse_scan,"\033[200~")){if(mode==9)mouse_used=0;else mouse_flush();mouse_paste=1;return;}
    if(!strcmp(mouse_scan,"\033[201~")){if(mode==9)mouse_used=0;else mouse_flush();mouse_paste=0;return;}
    if(!mouse_paste&&mouse_used>=9&&mouse_scan[2]=='<'&&(c=='M'||c=='m')) {
        for(i=3;i<mouse_used-1;i++) {
            unsigned char b=mouse_scan[i];
            if(b==';'){if(!digit||part==2)break;part++;digit=0;}
            else if(b>='0'&&b<='9'){if(values[part]>3276||(values[part]==3276&&b>'7'))break;values[part]=values[part]*10+b-'0';digit=1;}
            else break;
        }
        if(i==mouse_used-1&&part==2&&digit&&values[1]&&values[2]&&values[0]<128){mouse_used=0;mouse_route(values[0],values[1]-1,values[2]-1,c=='m');return;}
    }
    mouse_flush();
}
