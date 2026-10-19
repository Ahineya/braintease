/* SPDX-License-Identifier: GPL-2.0-only */
static void fill(int x,int y,int w,int h,GR_COLOR c)
{ if(w>0&&h>0) { GrSetGCForeground(gc,c); GrFillRect(wid,gc,x,y,w,h); } }
static plot_font_style_t ui_font(void)
{
    plot_font_style_t style;memset(&style,0,sizeof(style));
    style.family=PLOT_FONT_FAMILY_SANS_SERIF;style.size=9984;style.weight=400;return style;
}
static size_t ui_prefix(const char *s,size_t n,int budget,int *pixels)
{
    plot_font_style_t style=ui_font();size_t at=0,next;int total=0;unsigned advance;
    while(at<n) { advance=nf_advance(&style,s,n,at,&next);if(total+(long)advance>budget)break;total+=advance;at=next; }
    *pixels=total;return at;
}
static void label(int x,int y,const char *s,int max,GR_COLOR c)
{
    struct plot_context p;
    struct redraw_context redraw = { .plot = &plotters, .priv = &p };
    plot_font_style_t style = ui_font();
    int pixels;
    size_t n = strlen(s), shown;
    if (max <= 0) return;
    if (max > 128) max = 128;
    shown = ui_prefix(s,n,max*8,&pixels);
    p.limit.x0 = p.limit.y0 = 0; p.limit.x1 = width; p.limit.y1 = height;
    p.clip = p.limit; p.text_runs = p.rectangles = 0;
    style.foreground = c & 0xffffffUL;
    {
        nserror error=plot_text(&redraw,&style,x,y+12,s,shown);
        if(tracing&&x>width-100)fprintf(stderr,"NETSURF_GUI_LABEL x=%d clip=%d bytes=%lu result=%d\n",x,p.clip.x1,(unsigned long)shown,error);
    }
}
static int options_x(void) { return width>94?width-88:0; }
enum { BACK_RIGHT=46, NEXT_RIGHT=86, RELOAD_RIGHT=142, HOME_RIGHT=182, RANDOM_RIGHT=238 };
static bool random_visible(void) { return RANDOM_RIGHT<=options_x()-4; }
static void close_menu(void) { if(menu_open) { menu_open=false;full_damage();chrome_dirty=true; } }
static void update_transfer(void)
{
    uint64_t now;struct netsurf_transfer transfer;char text[160];
    if(nsu_getmonotonic_ms(&now)!=NSUERROR_OK)return;
    if((unsigned long)now-progress_stamp<125)return;
    progress_stamp=(unsigned long)now;
    netsurf_http_transfer(&transfer);text[0]=0;
    if(transfer.active) {
        if(!transfer.received)snprintf(text,sizeof(text),"Connecting / receiving headers: %s",transfer.url);
        else if(transfer.known)snprintf(text,sizeof(text),"Receiving %lu / %lu bytes (%lu%%)",transfer.received,transfer.total,
            transfer.total?transfer.received*100/transfer.total:100UL);
        else snprintf(text,sizeof(text),"Receiving %lu bytes (size unknown)",transfer.received);
    }
    if(strcmp(text,transfer_text)) {
        copy_text(transfer_text,sizeof(transfer_text),text);chrome_dirty=true;
        if(tracing&&text[0])fprintf(stderr,"NETSURF_GUI_TRANSFER %s\n",text);
    }
    /* Animate unknown-length transfers without forcing a document repaint. */
    if(transfer.active&&!transfer.known)chrome_dirty=true;
}
static void paint_menu(void)
{
    int mw=width>308?296:width-12,left=width-mw-6;
    GR_COLOR fg=GR_RGB(228,234,239), muted=GR_RGB(136,151,163);
    GrSetGCRegion(gc,0);
    fill(left+2,30,mw,184,GR_RGB(15,20,25));
    fill(left,28,mw,184,GR_RGB(44,55,65));
    fill(left+3,32+menu_selection*26,mw-6,26,GR_RGB(65,101,136));
    label(left+10,37,images_enabled?"[x] Images":"[ ] Images",(mw-20)/8,fg);
    label(left+10,63,"[ ] JavaScript (unavailable)",(mw-20)/8,muted);
    label(left+10,89,"Reload page",(mw-20)/8,fg);
    fill(left+8,116,mw-16,1,muted);
    label(left+10,129,c16_cache_enabled()?"Cache: disk + memory":"Cache: memory only",(mw-20)/8,muted);
    label(left+10,167,"Clear disk cache",(mw-20)/8,fg);
}
static void menu_action(void)
{
    int choice=menu_selection;
    close_menu();
    if(choice==0) {
        images_enabled=!images_enabled;
        nsoption_set_bool(foreground_images,images_enabled);
        nsoption_set_bool(background_images,images_enabled);
        browser_window_stop(window.bw);
        /* A changed policy requires rebuilding the HTML box tree. A normal
         * reload keeps reusable stylesheets/images rather than invalidating all. */
        browser_window_reload(window.bw,false);
        if(tracing)fprintf(stderr,"NETSURF_GUI_IMAGES %u\n",images_enabled?1:0);
    } else if(choice==2)browser_window_reload(window.bw,true);
    else if(choice==5)set_status(&window,c16_cache_clear()==NSERROR_OK?"Disk cache cleared.":"Disk cache is unavailable.");
}
static void paint_chrome(void)
{
    int ew,eh,track=view_height(), thumb,top;
    GR_COLOR bg=GR_RGB(44,55,65), fg=GR_RGB(228,234,239), muted=GR_RGB(136,151,163);
    GrSetGCRegion(gc,0);
    fill(0,0,width,TOOLBAR,bg);
    label(10,7,"Back",4,browser_window_history_back_available(window.bw)?fg:muted);
    label(BACK_RIGHT+4,7,"Next",4,browser_window_history_forward_available(window.bw)?fg:muted);
    label(NEXT_RIGHT+4,7,"Reload",6,fg); label(RELOAD_RIGHT+4,7,"Home",5,fg);
    if(random_visible())label(HOME_RIGHT+4,7,"Random",7,fg);
    fill(options_x()-4,3,88,25,menu_open?GR_RGB(65,101,136):bg);
    label(options_x(),7,"Options",7,fg);
    label(width-20,7,"v",1,fg);
    fill(6,32,width-12,26,editing?GR_RGB(224,237,248):GR_RGB(244,246,248));
    if(select_address)fill(10,37,width-20,16,GR_RGB(65,101,136));
    {
        int cap=(width-24)/8,pixels=0,total=0;plot_font_style_t style=ui_font();
        size_t n=strlen(address),start=0,next,at=0;
        while(at<n) { total+=nf_advance(&style,address,n,at,&next);at=next; }
        if(editing)while(total>cap*8-2&&start<n) { total-=nf_advance(&style,address,n,start,&next);start=next; }
        ui_prefix(address+start,n-start,cap*8,&pixels);
        label(10,37,address+start,cap,select_address?fg:GR_RGB(26,36,44));
        if(editing&&!select_address)fill(10+pixels,37,1,16,GR_RGB(26,36,44));
    }
    fill(0,height-STATUS,width,STATUS,bg);
    label(8,height-STATUS+3,transfer_text[0]?transfer_text:status_text,(width-16)/8,fg);
    if(transfer_text[0]) {
        struct netsurf_transfer t;int bar;
        netsurf_http_transfer(&t);
        bar=t.known?(t.total?(unsigned long)width*t.received/t.total:width):width/5;
        if(bar>width)bar=width;
        fill(t.known?0:(progress_stamp/125)%((unsigned)(width-bar)+1),height-2,bar,2,GR_RGB(89,190,140));
    }
    fill(view_width(),TOOLBAR,SCROLLBAR,track,GR_RGB(205,212,217));
    ew=view_width(); eh=track; browser_window_get_extents(window.bw,true,&ew,&eh);
    if(eh<track)eh=track;
    thumb=(long)track*track/eh; if(thumb<16)thumb=16; if(thumb>track)thumb=track;
    top=eh>track?(long)window.sy*(track-thumb)/(eh-track):0;
    fill(view_width()+3,TOOLBAR+top,SCROLLBAR-6,thumb,GR_RGB(99,119,135));
}
static void paint(void)
{
    if(!wid || (!dirty&&!chrome_dirty))return;
    if(dirty) {
        struct plot_context p;
        struct redraw_context redraw={.interactive=true,.background_images=true,.plot=&plotters,.priv=&p};
        p.limit=p.clip=damage; p.text_runs=p.rectangles=0;
        dirty=false;
        if(browser_window_has_content(window.bw))browser_window_redraw(window.bw,-window.sx,TOOLBAR-window.sy,&p.limit,&redraw);
        else { plot_clip(&redraw,&p.limit); fill(p.limit.x0,p.limit.y0,p.limit.x1-p.limit.x0,p.limit.y1-p.limit.y0,GR_RGB(255,255,255)); }
        if(caret&&!editing) {
            struct rect limit=viewport(); plot_clip(&redraw,&limit);
            fill(caret_x-window.sx,TOOLBAR+caret_y-window.sy,1,caret_height,GR_RGB(0,0,0));
        }
        paints++;
        if(tracing)fprintf(stderr,"NETSURF_GUI_PAINT %u text=%u rect=%u\n",paints,p.text_runs,p.rectangles);
    }
    if(chrome_dirty) { paint_chrome(); chrome_dirty=false; }
    if(menu_open)paint_menu();
    GrSetGCRegion(gc,0); GrFlush();
}
static void navigate(const char *value)
{
    nsurl *url=NULL;
    char path[sizeof(address)];
    nserror error;
    if(browser_address(value,path,sizeof(path))) {
        set_status(&window,"Invalid or overlong address."); return;
    }
    error=nsurl_create(path,&url);
    if(error==NSERROR_OK) {
        editing=select_address=false;
        error=browser_window_navigate(window.bw,url,NULL,BW_NAVIGATE_HISTORY,NULL,NULL,NULL);
        nsurl_unref(url);
    }
    if(error!=NSERROR_OK)set_status(&window,"Unable to open that address.");
    chrome_dirty=true;
}
static void focus_address(void) { close_menu(); editing=select_address=true; chrome_dirty=true; }
static void key_press(uint32_t key,unsigned modifiers)
{
    uint32_t nskey=key;
    if(key==NXR_KEY_COPY) { key='c';modifiers=MWKMOD_CTRL; }
    else if(key==NXR_KEY_PASTE) { key='v';modifiers=MWKMOD_CTRL; }
    else if(key==NXR_KEY_CUT) { key='x';modifiers=MWKMOD_CTRL; }
    else if(key==NXR_KEY_SELECT_ALL) { key='a';modifiers=MWKMOD_CTRL; }
    nskey=key;
    if(tracing)fprintf(stderr,"NETSURF_GUI_KEY %lu %u\n",(unsigned long)key,modifiers);
    bool ctrl=(modifiers&MWKMOD_CTRL)!=0, shift=(modifiers&MWKMOD_SHIFT)!=0;
    if(ctrl&&(key=='q'||key=='Q'||key==17)) { running=false; return; }
    if(ctrl&&(key=='l'||key=='L'||key==12||key=='o'||key=='O'||key==15)) { focus_address(); return; }
    if((modifiers&MWKMOD_ALT)&&(key=='o'||key=='O')) {
        if(menu_open)close_menu();else { menu_open=true;menu_selection=0;chrome_dirty=true; }
        return;
    }
    if(menu_open) {
        if(key==MWKEY_ESCAPE)close_menu();
        else if(key==MWKEY_UP||key==MWKEY_DOWN) { menu_selection=key==MWKEY_DOWN?(menu_selection==0?2:menu_selection==2?5:0):(menu_selection==0?5:menu_selection==5?2:0);chrome_dirty=true; }
        else if(key==MWKEY_ENTER||key==' ')menu_action();
        return;
    }
    if(modifiers&MWKMOD_ALT) {
        if(key==MWKEY_LEFT)browser_window_history_back(window.bw,false);
        if(key==MWKEY_RIGHT)browser_window_history_forward(window.bw,false);
        return;
    }
    if(key==MWKEY_F5) { browser_window_reload(window.bw,true); return; }
    if(editing) {
        size_t n=strlen(address);
        if(key==MWKEY_ESCAPE) { editing=select_address=false; set_url(&window,browser_window_access_url(window.bw)); }
        else if(key==MWKEY_ENTER)navigate(address);
        else if(ctrl&&(key=='a'||key=='A'||key==1))select_address=true;
        else if(ctrl&&(key=='c'||key=='C'||key==3||key=='x'||key=='X'||key==24)) {
            if(select_address&&copy_to_clipboard(address,n)==0&&(key=='x'||key=='X'||key==24))address[0]=0;
        } else if(ctrl&&(key=='v'||key=='V'||key==22)) {
            size_t length,start=0,end;char *text=NULL;clipboard_get(&text,&length);
            if(text) {
                end=length;
                while(start<end&&(unsigned char)text[start]<=32)start++;
                while(end>start&&(unsigned char)text[end-1]<=32)end--;
                for(length=start;length<end;length++)if((unsigned char)text[length]<32||(unsigned char)text[length]==127)break;
                if(select_address)n=0;
                if(length!=end)set_status(&window,"Paste a single-line address.");
                else if(end-start>=sizeof(address)-n)set_status(&window,"Pasted address is too long.");
                else { memcpy(address+n,text+start,end-start);address[n+end-start]=0;select_address=false; }
                free(text);
            }
        }
        else if(key==MWKEY_BACKSPACE||key==MWKEY_DELETE) {
            if(select_address)address[0]=0;
            else if(n)address[ru_previous_grapheme((const unsigned char *)address,n,n)]=0;
            select_address=false;
        } else if(!ctrl&&key>=32&&!(key>=MWKEY_FIRST&&key<=MWKEY_LAST)) {
            ns_octet_t encoded[6], *out=encoded; size_t remaining=sizeof(encoded), bytes;
            if(parserutils_charset_utf8_from_ucs4(key,&out,&remaining)==PARSERUTILS_OK) {
                bytes=sizeof(encoded)-remaining;
                if(select_address) { address[0]=0; n=0; }
                if(n+bytes<sizeof(address)) { memcpy(address+n,encoded,bytes); address[n+bytes]=0; }
                select_address=false;
            }
        }
        chrome_dirty=true; return;
    }
    switch(key) {
    case MWKEY_LEFT: nskey=NS_KEY_LEFT; break; case MWKEY_RIGHT: nskey=NS_KEY_RIGHT; break;
    case MWKEY_UP: nskey=NS_KEY_UP; break; case MWKEY_DOWN: nskey=NS_KEY_DOWN; break;
    case MWKEY_HOME: nskey=NS_KEY_LINE_START; break; case MWKEY_END: nskey=NS_KEY_LINE_END; break;
    case MWKEY_PAGEUP: nskey=NS_KEY_PAGE_UP; break; case MWKEY_PAGEDOWN: nskey=NS_KEY_PAGE_DOWN; break;
    case MWKEY_DELETE: nskey=NS_KEY_DELETE_RIGHT; break;
    case MWKEY_TAB: nskey=shift?NS_KEY_SHIFT_TAB:NS_KEY_TAB; break;
    default: if(ctrl&&key>='a'&&key<='z')nskey=key-'a'+1; break;
    }
    if(key>=MWKEY_FIRST&&key<=MWKEY_LAST&&nskey==key)return;
    if(browser_window_key_press(window.bw,nskey))return;
    if(key==MWKEY_UP)scroll_to(window.sx,window.sy-32);
    if(key==MWKEY_DOWN)scroll_to(window.sx,window.sy+32);
    if(key==MWKEY_LEFT)scroll_to(window.sx-32,window.sy);
    if(key==MWKEY_RIGHT)scroll_to(window.sx+32,window.sy);
    if(key==MWKEY_PAGEUP)scroll_to(window.sx,window.sy-view_height()+16);
    if(key==MWKEY_PAGEDOWN||key==' ')scroll_to(window.sx,window.sy+view_height()-16);
    if(key==MWKEY_HOME)scroll_to(0,0);
    if(key==MWKEY_END)scroll_to(window.sx,32700);
    if(key==MWKEY_ESCAPE)browser_window_stop(window.bw);
}
static bool pressed, dragging;
static int press_x,press_y;
static browser_mouse_state mouse_modifiers(unsigned modifiers)
{
    browser_mouse_state m=0;
    if(modifiers&MWKMOD_SHIFT)m|=BROWSER_MOUSE_MOD_1;
    if(modifiers&MWKMOD_CTRL)m|=BROWSER_MOUSE_MOD_2;
    if(modifiers&MWKMOD_ALT)m|=BROWSER_MOUSE_MOD_3;
    return m;
}
static void mouse_button(int x,int y,unsigned changed,unsigned modifiers,bool down)
{
    browser_mouse_state m=mouse_modifiers(modifiers);
    if(menu_open) {
        int mw=width>308?296:width-12,left=width-mw-6;
        if(down&&(changed&GR_BUTTON_L)) {
            if(x>=left&&x<left+mw&&y>=32&&y<188) {
                int row=(y-32)/26;
                if(row==0||row==2||row==5) { menu_selection=row;menu_action(); }
            } else close_menu();
        }
        return;
    }
    if(down&&(changed&GR_BUTTON_L)&&y<32&&x>=options_x()-4) {
        menu_open=true;menu_selection=0;chrome_dirty=true;return;
    }
    if(down&&(changed&GR_BUTTON_SCROLLUP)) { scroll_to(window.sx,window.sy-48); return; }
    if(down&&(changed&GR_BUTTON_SCROLLDN)) { scroll_to(window.sx,window.sy+48); return; }
    if(!(changed&GR_BUTTON_L))return;
    if(y>=32&&y<TOOLBAR) { if(down)focus_address(); return; }
    if(y<32) {
        if(!down)return;
        if(x<BACK_RIGHT)browser_window_history_back(window.bw,false);
        else if(x<NEXT_RIGHT)browser_window_history_forward(window.bw,false);
        else if(x<RELOAD_RIGHT)browser_window_reload(window.bw,true);
        else if(x<HOME_RIGHT)navigate(HOME);
        else if(x<RANDOM_RIGHT&&random_visible())navigate(RANDOM_URL);
        return;
    }
    if(y>=height-STATUS)return;
    if(x>=view_width()) {
        if(down) {
            int ew,eh; browser_window_get_extents(window.bw,true,&ew,&eh);
            scroll_to(window.sx,(long)(y-TOOLBAR)*eh/view_height()-view_height()/2);
        }
        return;
    }
    editing=false; chrome_dirty=true;
    if(down) {
        pressed=true; dragging=false; press_x=x; press_y=y;
        browser_window_mouse_click(window.bw,m|BROWSER_MOUSE_PRESS_1,x+window.sx,y-TOOLBAR+window.sy);
    } else if(pressed) {
        pressed=false;
        if(!dragging)browser_window_mouse_click(window.bw,m|BROWSER_MOUSE_CLICK_1,x+window.sx,y-TOOLBAR+window.sy);
        browser_window_mouse_track(window.bw,BROWSER_MOUSE_HOVER,x+window.sx,y-TOOLBAR+window.sy);
        dragging=false;
    }
}
static void input_key(GR_EVENT_KEYSTROKE *event)
{
    static ns_octet_t pending[4];
    static unsigned count,need,modifiers;
    unsigned ch=event->ch;
    uint32_t scalar; size_t bytes;
    if(event->scancode||ch<128||ch>255) {
        if(count) { count=need=0; key_press(0xfffdUL,modifiers); }
        key_press(ch,event->modifiers); return;
    }
    if(!count) {
        need=ch>=0xc2&&ch<=0xdf?2:ch>=0xe0&&ch<=0xef?3:ch>=0xf0&&ch<=0xf4?4:0;
        modifiers=event->modifiers;
        if(!need) { key_press(0xfffdUL,modifiers); return; }
    } else if((ch&0xc0)!=0x80) {
        count=need=0; key_press(0xfffdUL,modifiers); input_key(event); return;
    }
    pending[count++]=ch;
    if(count==need) {
        if(parserutils_charset_utf8_to_ucs4(pending,count,&scalar,&bytes)!=PARSERUTILS_OK ||
           scalar>0x10ffffUL || (scalar>=0xd800&&scalar<=0xdfff) ||
           (count==3&&scalar<0x800) || (count==4&&scalar<0x10000UL))scalar=0xfffdUL;
        count=need=0; key_press(scalar,modifiers);
    }
}
static void handle_event(GR_EVENT *event)
{
    switch(event->type) {
    case GR_EVENT_TYPE_CLOSE_REQ: running=false; break;
    case GR_EVENT_TYPE_EXPOSURE: full_damage(); chrome_dirty=true; break;
    case GR_EVENT_TYPE_UPDATE:
        if(event->update.utype==GR_UPDATE_SIZE) {
            close_menu();
            width=event->update.width; height=event->update.height;
            if(width<40)width=40; if(height<100)height=100;
            browser_window_schedule_reformat(window.bw); full_damage(); chrome_dirty=true;
            if(tracing)fprintf(stderr,"NETSURF_GUI_RESIZE %d %d\n",width,height);
        }
        break;
    case GR_EVENT_TYPE_KEY_DOWN: input_key(&event->keystroke); break;
    case GR_EVENT_TYPE_BUTTON_DOWN: case GR_EVENT_TYPE_BUTTON_UP:
        mouse_button(event->button.x,event->button.y,event->button.changebuttons,event->button.modifiers,event->type==GR_EVENT_TYPE_BUTTON_DOWN); break;
    case GR_EVENT_TYPE_MOUSE_POSITION: {
        int x=event->mouse.x,y=event->mouse.y;
        browser_mouse_state m=mouse_modifiers(event->mouse.modifiers);
        if(menu_open) {
            int mw=width>308?296:width-12,left=width-mw-6;
            if(x>=left&&x<left+mw&&y>=32&&y<188&&((y-32)/26==0||(y-32)/26==2||(y-32)/26==5)) {
                int row=(y-32)/26;
                if(menu_selection!=row) { menu_selection=row;chrome_dirty=true; }
            }
            break;
        }
        if(y<TOOLBAR||y>=height-STATUS||x>=view_width())break;
        if(pressed) {
            if(!dragging&&(abs(x-press_x)>3||abs(y-press_y)>3)) {
                browser_window_mouse_track(window.bw,m|BROWSER_MOUSE_DRAG_1,press_x+window.sx,press_y-TOOLBAR+window.sy); dragging=true;
            }
            if(dragging)m|=BROWSER_MOUSE_DRAG_ON|BROWSER_MOUSE_HOLDING_1;
        }
        browser_window_mouse_track(window.bw,m,x+window.sx,y-TOOLBAR+window.sy); break;
    }
    case GR_EVENT_TYPE_MOUSE_EXIT:
        pressed=dragging=false; browser_window_mouse_track(window.bw,BROWSER_MOUSE_LEAVE,0,0); break;
    }
}
