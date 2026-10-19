/* Native Nano-X windowport for NetHack 3.6.7. See ../upstream/dat/license. */
#include "hack.h"
#include "dlb.h"
#include "func_tab.h"
#include "ui.h"

#define NX_WINDOWS 32
#define NX_HISTORY 200
struct nx_line {
    char *text;
    anything id;
    char accelerator, group;
    int selected;
    long count;
};
struct nx_window {
    int type, cursor, used, capacity;
    struct nx_line *lines;
    char *title;
};
struct nx_view { const char **text; int *item, *start, used, capacity; };
static struct nx_window windows[NX_WINDOWS];
static char *history[NX_HISTORY];
static int history_count, history_iterator, unread, prompting, opened, close_pending, tracing;
extern short glyph2tile[];
extern void substitute_tiles(d_level *);
static void nx_putstr(winid,int,const char *);
static winid nx_create(int);
static void nx_destroy(winid);
static int nx_select(winid,int,menu_item **);
static void nx_getlin(const char *,char *);
static char nx_yn(const char *,const char *,char);

static void nx_bail(void)
{ clearlocks();nhnx_close();nh_terminate(EXIT_SUCCESS); }
static struct nx_window *get_window(winid id)
{
    if(id<0||id>=NX_WINDOWS||!windows[id].type)panic("Invalid Nano-X window %d",id);
    return &windows[id];
}
static void free_lines(struct nx_window *w)
{
    int i;for(i=0;i<w->used;i++)free(w->lines[i].text);
    free(w->lines);free(w->title);w->lines=0;w->title=0;w->used=w->capacity=w->cursor=0;
}
static struct nx_line *append(struct nx_window *w,const char *text)
{
    struct nx_line *line;int capacity;size_t at=0;const char *source;
    if(w->used>=16000)panic("Nano-X text window is too large");
    if(w->used==w->capacity){
        capacity=w->capacity?w->capacity*2:16;
        line=(struct nx_line *)alloc((size_t)capacity*sizeof(*line));
        if(w->used)memcpy(line,w->lines,(size_t)w->used*sizeof(*line));
        free(w->lines);w->lines=line;w->capacity=capacity;
    }
    line=&w->lines[w->used++];memset(line,0,sizeof(*line));
    source=text?text:"";line->text=(char *)alloc(strlen(source)*8+1);
    for(;*source;source++){
        if(*source=='\t'){do{line->text[at++]=' ';}while(at%8);}
        else line->text[at++]=*source;
    }
    line->text[at]=0;line->count=-1L;return line;
}
static void remember(const char *text)
{
    if(history_count==NX_HISTORY){
        free(history[0]);memmove(history,history+1,(NX_HISTORY-1)*sizeof(*history));history_count--;
    }
    history[history_count++]=dupstr(text);
}
static void event(struct nx_input *input)
{
    for(;;){
        nhnx_input(input);
        if(input->key!=NX_CLOSE)return;
        if(!program_state.something_worth_saving||program_state.gameover)nx_bail();
        /* Unwind an open prompt/menu before asking the core to save at the
         * next command boundary. Never destroy a live game's window here. */
        close_pending=1;input->key=27;return;
    }
}
static int plain_key(void)
{
    struct nx_input input;
    do { event(&input); } while(input.key==NX_RESIZE||input.key==NX_CLICK);
    if(input.key<0)return 27;
    return input.key;
}
static void nx_init(int *argc,char **argv)
{
    int i;char path[PATHLEN];
    (void)argc;(void)argv;
    tracing=getenv("NETHACK_TRACE")!=0;
    if(tracing)fprintf(stderr,"NHNX_OPENING\n");
    snprintf(path,sizeof(path),"%s",fqname("nxtiles",DATAPREFIX,0));
    if(nhnx_open(path)<0){
        fprintf(stderr,"NetHack: start the Nano-X desktop first (startx), and keep nxtiles in nethack-data.\n");
        nhnx_close();nh_terminate(EXIT_FAILURE);
    }
    opened=1;atexit(nhnx_close);iflags.window_inited=TRUE;
    iflags.wc_tiled_map=TRUE;iflags.wc_ascii_map=FALSE;
    iflags.wc_tile_width=iflags.wc_tile_height=16;
    for(i=1;i<=4;i++)nhnx_message(copyright_banner_line(i));
    nhnx_paint();
    if(tracing)fprintf(stderr,"NHNX_READY\n");
}
static void nx_exit(const char *message)
{
    int i;(void)message;
    for(i=0;i<NX_WINDOWS;i++)if(windows[i].type)nx_destroy(i);
    for(i=0;i<history_count;i++)free(history[i]);history_count=0;
    nhnx_close();opened=0;iflags.window_inited=FALSE;
    if(tracing)fprintf(stderr,"NHNX_EXIT\n");
}
static void nx_noop(void) {}
static void nx_suspend(const char *message) { (void)message; }
static void nx_sync(void) { nhnx_paint(); }
static winid nx_create(int type)
{
    int i;for(i=0;i<NX_WINDOWS;i++)if(!windows[i].type){windows[i].type=type;return i;}
    panic("Too many Nano-X windows");return WIN_ERR;
}
static void nx_clear(winid id)
{
    struct nx_window *w=get_window(id);free_lines(w);
    if(w->type==NHW_MAP)nhnx_clear_map();
    if(w->type==NHW_MESSAGE){nhnx_clear_messages();unread=0;}
    if(w->type==NHW_STATUS){nhnx_status(0,"");nhnx_status(1,"");}
}
static void nx_destroy(winid id)
{ struct nx_window *w=get_window(id);free_lines(w);memset(w,0,sizeof(*w)); }
static void nx_curs(winid id,int x,int y)
{
    struct nx_window *w=get_window(id);w->cursor=y;
    if(w->type==NHW_MAP)nhnx_focus(x,y);
}
static void nx_putstr(winid id,int attr,const char *s)
{
    struct nx_window *w;(void)attr;
    if(id==WIN_ERR){remember(s);if(opened)nhnx_message(s);return;}
    w=get_window(id);
    if(w->type==NHW_STATUS){nhnx_status(w->cursor,s);return;}
    if(w->type==NHW_MESSAGE){
        if(!s||!*s)return;
        if(unread>=3&&!prompting){
            if(tracing)fprintf(stderr,"NHNX_MORE\n");
            nhnx_prompt("--More--  Space / Enter / Esc");
            while(1){int c=plain_key();if(c==' '||c=='\n'||c=='\r'||c==27)break;}
            nhnx_prompt("");unread=0;
        }
        remember(s);nhnx_message(s);unread++;
        if(tracing)fprintf(stderr,"NHNX_MESSAGE %s\n",s);
        return;
    }
    append(w,s);
}
static void view_add(struct nx_view *v,const char *text,int length,int item)
{
    const char **texts;int *items,capacity;char *copy;
    if(v->used>=30000)panic("Nano-X wrapped text is too large");
    if(v->used==v->capacity){
        capacity=v->capacity>=15000?30000:v->capacity?v->capacity*2:32;
        texts=(const char **)alloc((size_t)capacity*sizeof(*texts));items=(int *)alloc((size_t)capacity*sizeof(*items));
        if(v->used){memcpy(texts,v->text,(size_t)v->used*sizeof(*texts));memcpy(items,v->item,(size_t)v->used*sizeof(*items));}
        free(v->text);free(v->item);v->text=texts;v->item=items;v->capacity=capacity;
    }
    copy=(char *)alloc((size_t)length+1);memcpy(copy,text,length);copy[length]=0;
    v->text[v->used]=copy;v->item[v->used++]=item;
}
static void free_view(struct nx_view *v)
{ int i;for(i=0;i<v->used;i++)free((void *)v->text[i]);free(v->text);free(v->item);free(v->start);memset(v,0,sizeof(*v)); }
static void build_view(struct nx_view *v,struct nx_window *w,int how)
{
    int i,n,take,width=nhnx_columns();char *label;const char *s;struct nx_line *line;
    free_view(v);v->start=(int *)alloc(((size_t)w->used+1)*sizeof(*v->start));
    for(i=0;i<w->used;i++){
        line=&w->lines[i];v->start[i]=v->used;
        label=(char *)alloc(strlen(line->text)+24);
        if(how!=PICK_NONE&&line->id.a_void&&line->selected&&line->count>=0)snprintf(label,strlen(line->text)+24,"%c [%ld] %s",line->accelerator,line->count,line->text);
        else if(how!=PICK_NONE&&line->id.a_void)snprintf(label,strlen(line->text)+24,"%c %s %s",line->accelerator,line->selected?"[x]":"[ ]",line->text);
        else strcpy(label,line->text);
        s=label;
        do{
            n=strlen(s);take=n>width?width:n;
            if(n>take){int end=take;while(end>take/2&&s[end]!=' ')end--;if(end>take/2)take=end;}
            view_add(v,s,take,i);s+=take;while(*s==' ')s++;
        }while(*s);
        free(label);
    }
    v->start[w->used]=v->used;
    if(!v->used)view_add(v,"(empty)",7,-1);
}
static int next_item(struct nx_window *w,int focus,int direction)
{
    int i=focus;
    if(!w->used)return -1;
    do{
        i+=direction;if(i<0)i=w->used-1;if(i>=w->used)i=0;
        if(i<0)return -1;
        if(w->lines[i].id.a_void)return i;
    }while(i!=focus&&!(focus<0&&i==w->used-1));
    return -1;
}
static void nx_start_menu(winid id) { nx_clear(id); }
static void nx_add_menu(winid id,int glyph,const anything *identifier,char accelerator,char group,int attr,const char *s,boolean selected)
{
    struct nx_line *line=append(get_window(id),s);
    (void)glyph;(void)attr;
    if(identifier)line->id=*identifier;
    line->accelerator=accelerator;line->group=group;line->selected=selected;
}
static void nx_end_menu(winid id,const char *title)
{ struct nx_window *w=get_window(id);free(w->title);w->title=dupstr(title?title:"NetHack"); }
static int nx_select(winid id,int how,menu_item **result)
{
    struct nx_window *w=get_window(id);struct nx_view view;struct nx_input input;
    int i,k,focus=-1,top=0,rows,selection=-1,number=0,redraw=1,automatic=0,done=0;
    long quantity=-1L;
    memset(&view,0,sizeof(view));*result=0;prompting++;
    for(i=0;i<w->used;i++)if(w->lines[i].id.a_void){
        if(!w->lines[i].accelerator){int n=automatic++%52;w->lines[i].accelerator=n<26?'a'+n:'A'+n-26;}
        if(focus<0)focus=i;
    }
    if(tracing)fprintf(stderr,"NHNX_MENU %d %d %s\n",w->used,how,w->title?w->title:"Text");
    while(!done){
        if(redraw){build_view(&view,w,how);redraw=0;}
        rows=nhnx_rows();
        if(top>view.used-rows)top=view.used-rows;if(top<0)top=0;
        if(how!=PICK_NONE&&focus>=0&&(view.start[focus]<top||view.start[focus]>=top+rows)){
            for(i=top;i<view.used&&i<top+rows;i++)if(view.item[i]>=0&&w->lines[view.item[i]].id.a_void){focus=view.item[i];break;}
        }
        nhnx_overlay(w->title?w->title:"NetHack",view.text,view.used,top,how!=PICK_NONE&&focus>=0?view.start[focus]:-1,
            how==PICK_NONE?"Up/Down: scroll   Space: next page   Enter/Esc: close":
            how==PICK_ANY?"Arrows: move   Space: toggle   Enter: accept   Esc: cancel":"Arrows: move   Enter or letter: choose   Esc: cancel");
        event(&input);k=input.key;
        if(k==NX_RESIZE){redraw=1;continue;}
        if(k==27){selection=-1;break;}
        if(k==NX_CLICK){
            if(input.row>=0&&input.row<view.used){i=view.item[input.row];if(i>=0&&w->lines[i].id.a_void){focus=i;k=how==PICK_ANY?' ':'\n';}else continue;}
            else continue;
        }
        if(k==NX_UP||k==NX_DOWN){
            if(how==PICK_NONE){top+=k==NX_UP?-1:1;continue;}
            focus=next_item(w,focus,k==NX_UP?-1:1);
            if(focus>=0){if(view.start[focus]<top)top=view.start[focus];if(view.start[focus]>=top+rows)top=view.start[focus]-rows+1;}
            redraw=1;continue;
        }
        if(k==NX_PAGEUP||k==NX_LEFT||k==MENU_PREVIOUS_PAGE){top-=rows;continue;}
        if(k==NX_PAGEDOWN||k==NX_RIGHT||k==MENU_NEXT_PAGE||(k==' '&&how==PICK_NONE)){
            if(how==PICK_NONE&&top+rows>=view.used){selection=0;break;}top+=rows;continue;
        }
        if(k==NX_HOME||k==MENU_FIRST_PAGE){top=0;continue;}
        if(k==NX_END||k==MENU_LAST_PAGE){top=view.used-rows;continue;}
        if(k=='\n'||k=='\r'){
            if(how==PICK_NONE){selection=0;break;}
            if(how==PICK_ONE&&focus>=0){for(i=0;i<w->used;i++)w->lines[i].selected=0;w->lines[focus].selected=1;w->lines[focus].count=quantity;}
            selection=0;break;
        }
        if(how==PICK_NONE)continue;
        if(k>='0'&&k<='9'){
            if(!number){quantity=0;number=1;}
            if(quantity<=(2147483647L-(k-'0'))/10)quantity=quantity*10+k-'0';
            continue;
        }
        if(k==':'&&how!=PICK_NONE){
            char query[BUFSZ];nx_getlin("Find",query);
            if(query[0]!=27&&*query)for(i=0;i<w->used;i++)if(w->lines[i].id.a_void&&strstri(w->lines[i].text,query)){focus=i;top=view.start[i];break;}
            redraw=1;continue;
        }
        if(how==PICK_ANY&&(k=='.'||k=='-'||k=='@'||k==','||k=='\\'||k=='~')){
            for(i=0;i<w->used;i++)if(w->lines[i].id.a_void){
                if((k==','||k=='\\'||k=='~')&&(view.start[i]<top||view.start[i]>=top+rows))continue;
                w->lines[i].selected=(k=='.'||k==',')?1:(k=='-'||k=='\\')?0:!w->lines[i].selected;
                w->lines[i].count=-1L;
            }
            redraw=1;continue;
        }
        if(k==' '&&focus>=0)i=focus;
        else{
            /* Repeated automatic accelerators address the current page first. */
            i=-1;for(k=0;k<view.used;k++)if(k>=top&&k<top+rows&&view.item[k]>=0){
                int item=view.item[k];if(w->lines[item].id.a_void&&w->lines[item].accelerator==input.key){i=item;break;}
            }
            if(i<0)for(k=0;k<w->used;k++)if(w->lines[k].id.a_void&&w->lines[k].accelerator==input.key){i=k;break;}
        }
        if(i>=0){
            focus=i;w->lines[i].selected=!w->lines[i].selected;w->lines[i].count=number?quantity:-1L;
            quantity=-1L;number=0;redraw=1;
            if(how==PICK_ONE){for(k=0;k<w->used;k++)w->lines[k].selected=0;w->lines[i].selected=1;selection=0;break;}
        }else if(how==PICK_ANY){
            for(i=0;i<w->used;i++)if(w->lines[i].id.a_void&&w->lines[i].group&&w->lines[i].group==input.key){w->lines[i].selected=!w->lines[i].selected;w->lines[i].count=-1L;redraw=1;}
        }
    }
    nhnx_overlay(0,0,0,0,-1,0);nhnx_prompt("");free_view(&view);prompting--;unread=0;
    if(selection<0)return -1;
    for(i=0;i<w->used;i++)if(w->lines[i].id.a_void&&w->lines[i].selected)selection++;
    if(selection){
        *result=(menu_item *)alloc((size_t)selection*sizeof(**result));k=0;
        for(i=0;i<w->used;i++)if(w->lines[i].id.a_void&&w->lines[i].selected){(*result)[k].item=w->lines[i].id;(*result)[k++].count=w->lines[i].count;}
    }
    return selection;
}
static void nx_display(winid id,boolean blocking)
{
    struct nx_window *w=get_window(id);menu_item *ignored=0;
    if(w->type==NHW_TEXT||w->type==NHW_MENU){nx_select(id,PICK_NONE,&ignored);free(ignored);}
    else{
        nhnx_paint();
        /* The core uses a blocking message display to flush "Saving...";
         * graphical ports only wait on a blocking map/text window. */
        if(blocking&&w->type==NHW_MAP){unread=0;nhnx_prompt("Press a key to continue.");plain_key();nhnx_prompt("");}
    }
}
static void nx_display_file(const char *path,boolean complain)
{
    dlb *f=dlb_fopen(path,"r");char line[BUFSZ];winid win;
    if(!f){if(complain)pline("Cannot open %s.",path);return;}
    win=nx_create(NHW_TEXT);nx_end_menu(win,path);
    while(dlb_fgets(line,sizeof(line),f)){line[strcspn(line,"\r\n")]=0;nx_putstr(win,0,line);}
    dlb_fclose(f);nx_display(win,TRUE);nx_destroy(win);
}
static void nx_glyph(winid id,xchar x,xchar y,int glyph,int background)
{
    static d_level previous;static int valid;
    (void)id;(void)background;
    if(glyph<0||glyph>=MAX_GLYPH)return;
    if(!valid||previous.dnum!=u.uz.dnum||previous.dlevel!=u.uz.dlevel){substitute_tiles(&u.uz);previous=u.uz;valid=1;}
    nhnx_tile(x,y,glyph2tile[glyph],iflags.hilite_pet&&glyph_is_pet(glyph));
}
static void nx_raw(const char *s) { if(opened)nhnx_message(s);else puts(s); }
static int nx_getch(void) { unread=0;return plain_key(); }
static int direction_index(int key)
{
    switch(key){
    case NX_UP:return 2;case NX_DOWN:return 6;
    case NX_LEFT:return 0;case NX_RIGHT:return 4;
    case NX_HOME:return 1;case NX_END:return 7;
    case NX_PAGEUP:return 3;case NX_PAGEDOWN:return 5;
    }
    return -1;
}
static int direction_key(void)
{
    struct nx_input input;int index;
    for(;;){
        event(&input);index=direction_index(input.key);
        if(index>=0)return Cmd.dirchars[index];
        if(input.key==NX_CLICK||input.key==NX_RESIZE)continue;
        return input.key<0?27:input.key;
    }
}
static int nx_poskey(int *x,int *y,int *modifier)
{
    struct nx_input input;int index;
    unread=0;
    if(close_pending){close_pending=0;return 'S';}
    if(tracing)fprintf(stderr,"NHNX_TURN %ld %d %d\n",moves,u.ux,u.uy);
    for(;;){
        event(&input);index=direction_index(input.key);
        if(close_pending){close_pending=0;return 'S';}
        switch(input.key){
        case NX_RESIZE:continue;
        case NX_CLICK:if(input.x<1)continue;*x=input.x;*y=input.y;*modifier=CLICK_1;return 0;
        }
        if(index>=0)return Cmd.dirchars[index];
        if(input.key>=0)return input.key;
    }
}
static void nx_getlin(const char *question,char *output)
{
    int n=0,k;char text[BUFSZ*2];prompting++;output[0]=0;unread=0;
    if(tracing)fprintf(stderr,"NHNX_PROMPT %s\n",question);
    for(;;){
        snprintf(text,sizeof(text),"%s %s_",question,output);nhnx_prompt(text);k=plain_key();
        if(k==27){output[0]=27;output[1]=0;break;}
        if(k=='\n'||k=='\r')break;
        if(k==8||k==127){if(n)output[--n]=0;}
        else if(k==21){n=0;output[0]=0;}
        else if(k==23){while(n&&output[n-1]==' ')n--;while(n&&output[n-1]!=' ')n--;output[n]=0;}
        else if(k>=32&&k<127&&n<BUFSZ-1){output[n++]=k;output[n]=0;}
    }
    nhnx_prompt("");prompting--;
}
static char nx_yn(const char *question,const char *choices,char def)
{
    char text[BUFSZ*2],visible[BUFSZ];int k,n=0;long count;
    if(choices)while(choices[n]&&choices[n]!=27&&n<BUFSZ-1){visible[n]=choices[n];n++;}
    visible[n]=0;prompting++;unread=0;
    if(tracing)fprintf(stderr,"NHNX_PROMPT %s\n",question);
    snprintf(text,sizeof(text),"%s %s%s%s",question,choices?"[":"",visible,choices?"]":"");
    nhnx_prompt(text);
    for(;;){
        k=choices?plain_key():direction_key();
        if(!choices)break;
        if(k==27){k=strchr(choices,'q')?'q':strchr(choices,'n')?'n':def;break;}
        if((k==' '||k=='\n'||k=='\r')&&def){k=def;break;}
        if(k>='0'&&k<='9'&&strchr(choices,'#')){
            count=k-'0';
            for(;;){
                snprintf(text,sizeof(text),"%s #%ld",question,count);nhnx_prompt(text);k=plain_key();
                if(k=='\n'||k=='\r'){yn_number=count;k='#';break;}
                if(k==27){k=def;break;}
                if(k==8||k==127)count/=10;
                else if(k>='0'&&k<='9'&&count<=(2147483647L-(k-'0'))/10)count=count*10+k-'0';
            }
            break;
        }
        if(strchr(choices,k))break;
    }
    nhnx_prompt("");prompting--;return k;
}
static void nx_askname(void)
{
    char name[BUFSZ];
    do{nx_getlin("What is your name?",name);if(name[0]==27)nx_bail();mungspaces(name);}while(!*name);
    strncpy(plname,name,19);plname[19]=0;
}
static int pick_facet(int facet)
{
    winid win=nx_create(NHW_MENU);anything id;menu_item *selection=0;
    int i,n,count=0,valid;const char *label=0;
    nx_start_menu(win);
    for(i=0;;i++){
        if(facet==0){label=roles[i].name.m;if(!label)break;valid=ok_role(i,flags.initrace,flags.initgend,flags.initalign);}
        else if(facet==1){label=races[i].noun;if(!label)break;valid=ok_race(flags.initrole,i,flags.initgend,flags.initalign);}
        else if(facet==2){if(i>=ROLE_GENDERS)break;label=genders[i].adj;valid=ok_gend(flags.initrole,flags.initrace,i,flags.initalign);}
        else{if(i>=ROLE_ALIGNS)break;label=aligns[i].adj;valid=ok_align(flags.initrole,flags.initrace,flags.initgend,i);}
        if(valid){id=zeroany;id.a_int=i+1;nx_add_menu(win,NO_GLYPH,&id,0,0,0,label,FALSE);count++;}
    }
    id=zeroany;id.a_int=1000;nx_add_menu(win,NO_GLYPH,&id,'*',0,0,"Random",FALSE);
    nx_end_menu(win,facet==0?"Choose your role":facet==1?"Choose your race":facet==2?"Choose your gender":"Choose your alignment");
    n=nx_select(win,PICK_ONE,&selection);i=n>0?selection[0].item.a_int-1:ROLE_NONE;
    free(selection);nx_destroy(win);if(i==ROLE_NONE)nx_bail();return i==999?ROLE_RANDOM:i;
}
static void nx_player_selection(void)
{
    int manual,i;char answer;
    if(flags.initrole>=0&&flags.initrace>=0&&flags.initgend>=0&&flags.initalign>=0)return;
    for(;;){
        rigid_role_checks();manual=0;
        if(!flags.randomall){answer=nx_yn("Shall I pick a character for you?","ynq",'y');if(answer=='q')nx_bail();manual=answer=='n';}
        if(flags.initrole==ROLE_NONE&&manual)flags.initrole=pick_facet(0);
        if(flags.initrole<0){i=pick_role(flags.initrace,flags.initgend,flags.initalign,PICK_RANDOM);flags.initrole=i>=0?i:randrole(FALSE);}
        if(flags.initrace==ROLE_NONE&&manual)flags.initrace=pick_facet(1);
        if(flags.initrace<0)flags.initrace=pick_race(flags.initrole,flags.initgend,flags.initalign,PICK_RANDOM);
        if(flags.initrace<0)flags.initrace=randrace(flags.initrole);
        if(flags.initgend==ROLE_NONE&&manual)flags.initgend=pick_facet(2);
        if(flags.initgend<0)flags.initgend=pick_gend(flags.initrole,flags.initrace,flags.initalign,PICK_RANDOM);
        if(flags.initgend<0)flags.initgend=randgend(flags.initrole,flags.initrace);
        if(flags.initalign==ROLE_NONE&&manual)flags.initalign=pick_facet(3);
        if(flags.initalign<0)flags.initalign=pick_align(flags.initrole,flags.initrace,flags.initgend,PICK_RANDOM);
        if(flags.initalign<0)flags.initalign=randalign(flags.initrole,flags.initrace);
        if(flags.randomall)return;
        { char question[BUFSZ];snprintf(question,sizeof(question),"Play a %s %s %s %s?",aligns[flags.initalign].adj,genders[flags.initgend].adj,races[flags.initrace].adj,roles[flags.initrole].name.m);
          answer=nx_yn(question,"ynq",'y'); }
        if(answer=='y')return;if(answer=='q')nx_bail();
        flags.initrole=flags.initrace=flags.initgend=flags.initalign=ROLE_NONE;
    }
}
static int nx_ext_cmd(void)
{
    char command[BUFSZ];int i,found=-1,matches=0;nx_getlin("#",command);
    if(command[0]==27||!*command)return -1;mungspaces(command);
    for(i=0;extcmdlist[i].ef_txt;i++)if((wizard||!(extcmdlist[i].flags&WIZMODECMD))&&!(extcmdlist[i].flags&CMD_NOT_AVAILABLE)){
        if(!strcmpi(command,extcmdlist[i].ef_txt))return i;
        if(!strncmpi(command,extcmdlist[i].ef_txt,strlen(command))){found=i;matches++;}
    }
    if(matches==1)return found;
    pline(matches?"Ambiguous command: %s":"Unknown command: %s",command);return -1;
}
static int nx_previous(void)
{
    int i;winid win=nx_create(NHW_TEXT);nx_end_menu(win,"Message history");
    for(i=0;i<history_count;i++)nx_putstr(win,0,history[i]);nx_display(win,TRUE);nx_destroy(win);return 0;
}
static char *nx_history(boolean init)
{ if(init)history_iterator=0;return history_iterator<history_count?history[history_iterator++]:0; }
static void nx_restore_history(const char *message,boolean restoring)
{ (void)restoring;if(message){remember(message);nhnx_message(message);} }
static void nx_clip(int x,int y) { nhnx_focus(x,y); }
static void nx_number(int state) { (void)state; }
static void nx_preference(const char *option) { (void)option;nhnx_paint(); }
static void nx_status_init(void) { genl_status_init(); }

struct window_procs nx_procs = {
    "nano-x", WC_TILED_MAP|WC_COLOR|WC_HILITE_PET|WC_EIGHT_BIT_IN|WC_MOUSE_SUPPORT,
    WC2_FLUSH_STATUS|WC2_WRAPTEXT,
    {1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1},
    nx_init,nx_player_selection,nx_askname,nx_sync,nx_exit,nx_suspend,nx_sync,
    nx_create,nx_clear,nx_display,nx_destroy,nx_curs,nx_putstr,genl_putmixed,
    nx_display_file,nx_start_menu,nx_add_menu,nx_end_menu,nx_select,
    genl_message_menu,nx_noop,nx_sync,nx_sync,
#ifdef CLIPPING
    nx_clip,
#endif
#ifdef POSITIONBAR
    nx_number,
#endif
    nx_glyph,nx_raw,nx_raw,nx_getch,nx_poskey,nx_noop,nx_previous,nx_yn,
    nx_getlin,nx_ext_cmd,nx_number,nx_noop,
#ifdef CHANGE_COLOR
    0,
#ifdef MAC
    0,0,
#endif
    0,
#endif
    nx_noop,nx_noop,genl_outrip,nx_preference,nx_history,nx_restore_history,
    nx_status_init,genl_status_finish,genl_status_enablefield,genl_status_update,
    genl_can_suspend_no
};
