/* Prefix commands leave ordinary input, including Ctrl-C/Z and Tab, to the PTY. */
static unsigned escape_state,escape_param;
static unsigned long escape_deadline;
static unsigned long milliseconds(void){struct timespec t;if(clock_gettime(CLOCK_MONOTONIC,&t))return 0;return t.tv_sec*1000UL+t.tv_nsec/1000000UL;}
static void copy_key(unsigned c);
static void escape_timeout(void){if(escape_state&&(long)(milliseconds()-escape_deadline)>=0){escape_state=0;if(mode==1)copy_key(27);else{prefix_pending=0;dirty=1;}}}
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
    n=QUEUE-p->queued;if(n>paste_count)n=paste_count;
    enqueue(p,clipboard+paste_at,n);paste_at+=n;paste_count-=n;
}
static void prompt_input(unsigned c){
    struct stat st;unsigned limit=mode==2?24:mode==3?sizeof(search)-1:mode==5?DIRECTORY_SIZE-1:RESTART_SIZE-1;
    if(c==27||c==3){mode=mode==3?1:0;prompt_len=0;full=dirty=1;return;}
    if(c=='\r'||c=='\n'){
        if(mode==2){if(prompt_len)snprintf(tabs[current].name,sizeof(tabs[current].name),"%s",prompt);mode=0;}
        else if(mode==3){strcpy(search,prompt);mode=1;find_history();}
        else if(mode==4){strcpy(focused()->restart,prompt);mode=0;notice("Restart command updated; prefix S saves the session");}
        else if(mode==5){
            mode=0;
            if(prompt[0]!='/'||stat(prompt,&st)||!S_ISDIR(st.st_mode))notice("Restart directory must be an existing absolute directory");
            else{strcpy(focused()->directory,prompt);notice("Restart directory updated; prefix S saves the session");}
        }
        full=dirty=1;return;
    }
    if(c==21){prompt_len=0;prompt[0]=0;}
    else if(c==127||c==8){if(prompt_len)prompt[--prompt_len]=0;}
    else if(c>=32&&c<127&&prompt_len<limit){prompt[prompt_len++]=c;prompt[prompt_len]=0;}
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
    prefix_pending=0;dirty=1;message[0]=0;
    if(c==options.prefix){char b=c;enqueue(focused(),&b,1);}
    else if(c=='c')new_tab(NULL);
    else if(c=='n')cycle_tab(1);else if(c=='p')cycle_tab(-1);
    else if(c>='1'&&c<='8'){if(tabs[c-'1'].used){current=c-'1';need_layout=1;mode=0;}else notice("That tab is not open");}
    else if(c=='%')split_pane(1);else if(c=='"')split_pane(0);
    else if(c=='o'||c==9)cycle_pane();
    else if(c=='h')focus_direction(-1,0);else if(c=='l')focus_direction(1,0);
    else if(c=='k')focus_direction(0,-1);else if(c=='j')focus_direction(0,1);
    else if(c=='+')adjust_split(5);else if(c=='-')adjust_split(-5);
    else if(c=='z'){tabs[current].zoom=!tabs[current].zoom;need_layout=1;}
    else if(c=='!')break_pane();
    else if(c=='S'){if(save_session())notice("Cannot save session in ~/.mux");}
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
static void decoded_arrow(unsigned c){
    if(mode==1){if(c=='A')copy_key('k');else if(c=='B')copy_key('j');else if(c=='H')copy_key('g');else if(c=='F')copy_key('G');}
    else if(prefix_pending){if(c=='A')command_key('k');else if(c=='B')command_key('j');else if(c=='C')command_key('l');else if(c=='D')command_key('h');}
}
static void handle_input(unsigned c){
    char b=c;
    if(help){help=0;full=dirty=1;return;}
    if(closing){closing=0;if(c=='y'||c=='Y')remove_pane(tabs[current].focus);else dirty=1;return;}
    if(mode>=2){prompt_input(c);return;}
    /* Only interpret escape keys in mux modes. App input otherwise stays raw. */
    if((mode==1||prefix_pending)&&escape_state){
        if(escape_state==1){if(c=='['||c=='O'){escape_state=2;escape_param=0;return;}escape_state=0;copy_key(27);return;}
        if(c>='0'&&c<='9'){escape_param=escape_param*10+c-'0';if(escape_param>999)escape_state=0;return;}
        escape_state=0;
        if(c=='~'){if(mode==1){if(escape_param==5)copy_key('b');else if(escape_param==6)copy_key('f');}}
        else decoded_arrow(c);return;
    }
    if((mode==1||prefix_pending)&&c==27){escape_state=1;escape_deadline=milliseconds()+100;return;}
    if(prefix_pending){command_key(c);return;}
    if(mode==1){copy_key(c);return;}
    if(c==options.prefix){prefix_pending=1;dirty=1;return;}
    if(message[0]){message[0]=0;dirty=1;}
    /* Console input may encode Backspace as ^H, while GUI/Telnet use DEL.
     * Panes consistently advertise DEL; translate the attaching tty erase key. */
    if(c==erase_character&&(c==8||c==127))b=127;
    if(enqueue(focused(),&b,1))notice("Pane input queue is full; waiting for the application");
}
