/* Native GX desktop adapter. Only decoded numbers and validated synchronous
 * C16 spans reach this layer. Window policy is the shared Rust implementation.
 * Entry is serialized by existing checkpoints; IRQs never call this adapter. */
#include "../graphics/desktop-core.h"
#define SESSION_PROCESSES 128
#define PANEL_REGIONS 16
typedef struct {uint32_t id,instance,leader;uint64_t owner;unsigned pid;char app[64],name[64],icon[128];} DesktopProcess;
typedef struct {unsigned x,y,w,h;} PanelRect;
static DesktopProcess desktop_processes[SESSION_PROCESSES];
static uint32_t desktop_revision,desktop_instance;
static uint32_t *desktop_pixels;
static unsigned desktop_buttons,desktop_keys[16],desktop_hover;
static uint32_t panel_window,panel_process;
static unsigned panel_flags,panel_top,panel_bottom,panel_count,panel_capture;
static PanelRect panel_regions[PANEL_REGIONS];
static uint16_t desktop_clipboard[32768];
static unsigned desktop_clipboard_length;
static int32_t gx_basic_call(uint64_t,unsigned,uint32_t[12],const GxGuest *);
static int32_t desktop_call(uint64_t,unsigned,uint32_t[12],const GxGuest *);
static void desktop_compose(void);
static int desktop_panel_active(void);
static Surface *desktop_surface(uint32_t id) {
    for(unsigned i=0;i<WINDOWS;i++)if(id&&surfaces[i].handle==id)return &surfaces[i];return 0;
}
static void desktop_changed(void) { if(desktop_revision!=UINT32_MAX)++desktop_revision; }
static DesktopProcess *desktop_process(uint32_t id) {
    for(unsigned i=0;i<SESSION_PROCESSES;i++)if(id&&desktop_processes[i].id==id)return &desktop_processes[i];return 0;
}
static DesktopProcess *desktop_touch(uint32_t id,unsigned pid,uint64_t owner) {
    DesktopProcess *p=desktop_process(id);
    if(!id||pid>65535)return 0;
    if(p){if(!p->owner)p->owner=owner;return p;}
    for(unsigned i=0;i<SESSION_PROCESSES;i++)if(!desktop_processes[i].id){
        p=&desktop_processes[i];zero(p,sizeof(*p));p->id=id;p->pid=pid;p->owner=owner;desktop_changed();return p;
    }
    return 0;
}
static unsigned utf8_valid(const uint16_t *s,size_t n) {
    size_t i=0;
    while(i<n){unsigned c=s[i++],v,extra,min;
        if(c<128)continue;
        if(c>=0xc2&&c<=0xdf){v=c&31;extra=1;min=128;}
        else if(c>=0xe0&&c<=0xef){v=c&15;extra=2;min=2048;}
        else if(c>=0xf0&&c<=0xf4){v=c&7;extra=3;min=65536;}else return 0;
        if(extra>n-i)return 0;
        while(extra--){c=s[i++];if(c<128||c>191)return 0;v=(v<<6)|(c&63);}
        if(v<min||v>0x10ffff||(v>=0xd800&&v<=0xdfff))return 0;
    }return 1;
}
static int desktop_text_cells(const uint16_t *p,char *out,unsigned capacity) {
    unsigned n=0;while(n<capacity&&p[n]){if(p[n]>255)return EINVAL_;++n;}
    if(n==capacity||!utf8_valid(p,n))return EINVAL_;
    for(unsigned i=0;i<=n;i++)out[i]=(char)p[i];return 0;
}
static int desktop_text(const GxGuest *g,uint32_t address,char *out,unsigned capacity) {
    uint16_t *p=span(g,address,capacity,0);return p?desktop_text_cells(p,out,capacity):EFAULT_;
}
static unsigned text_length(const char *s) {unsigned n=0;while(s[n])++n;return n;}
static int text_equal(const char *a,const char *b) {while(*a&&*a==*b){++a;++b;}return *a==*b;}
static void text_copy(char *out,const char *in,unsigned n){for(unsigned i=0;i<n;i++){out[i]=in[i];if(!in[i])break;}}
static void desktop_event(Surface *s,unsigned type,unsigned flags,int32_t x,int32_t y) {
    enqueue(s,(Event){.type=type,.flags=flags,.buttons=s->buttons,.x=x,.y=y});
}
static void desktop_action(uint32_t kind,uint32_t id,int32_t a,int32_t b,int32_t c,int32_t d) {
    Surface *s=desktop_surface(id);if(!s)return;
    if(kind==1){
        unsigned focused=a&&!(desktop_panel_active()&&(panel_flags&1));
        if(s->focused!=focused){s->focused=focused;zero(s->held,sizeof(s->held));s->buttons=0;
            desktop_event(s,6,focused?8:0,0,0);desktop_changed();}
    } else if(kind==2){
        if(s->pending_width!=(unsigned)a||s->pending_height!=(unsigned)b){
            s->pending_width=a;s->pending_height=b;desktop_event(s,10,0,a,b);desktop_changed();}
    } else if(kind==3){s->closing=1;desktop_event(s,8,0,0,0);desktop_changed();}
    else if(kind==4){s->captured=a!=0;desktop_event(s,7,a?2:0,0,0);}
    else if(kind==5){s->x=a;s->y=b;desktop_event(s,2,0,a,b);}
    else if(kind==6){unsigned button=(unsigned)a+1;if(b)s->buttons|=1U<<a;else s->buttons&=~(1U<<a);
        enqueue(s,(Event){.type=3,.flags=b?1:0,.code=button,.buttons=s->buttons,.x=c,.y=d});}
}
static void desktop_init(void) {
    zero(desktop_processes,sizeof(desktop_processes));zero(desktop_keys,sizeof(desktop_keys));
    desktop_revision=1;desktop_instance=0;desktop_buttons=desktop_hover=0;
    panel_window=panel_process=panel_flags=panel_top=panel_bottom=panel_count=panel_capture=0;
    desktop_pixels=0;desktop_clipboard_length=0;
    xd_init(platform.width,platform.height,desktop_action);
}
static int desktop_panel_active(void){Surface *s=desktop_surface(panel_window);return s&&s->mapped&&!(s->flags&2)&&panel_count;}
static int desktop_active(void) {
    if(desktop_panel_active())return 1;
    for(unsigned i=0;i<WINDOWS;i++)if(surfaces[i].handle&&surfaces[i].mapped&&!(surfaces[i].flags&2))return 1;
    return 0;
}
static int desktop_panel_hit(int32_t x,int32_t y) {
    if(!desktop_panel_active())return 0;
    if(panel_flags&2)return 1;
    for(unsigned i=0;i<panel_count;i++){PanelRect *r=panel_regions+i;
        if(x>=(int32_t)r->x&&y>=(int32_t)r->y&&x<(int32_t)(r->x+r->w)&&y<(int32_t)(r->y+r->h)){
            Surface *s=desktop_surface(panel_window);
            return !(panel_flags&4)||(s->front[(size_t)y*s->width+x]>>24)>=128;
        }
    }return 0;
}
static void desktop_panel_sync(void) {
    unsigned active=desktop_panel_active();
    Surface *panel=desktop_surface(panel_window);
    if(!active&&panel){
        panel_capture=0;panel->buttons=0;
        if(panel->captured){panel->captured=0;desktop_event(panel,7,0,panel->x,panel->y);}
    }
    xd_edges(active?panel_top:0,active?panel_bottom:34,active);
    for(unsigned i=0;i<WINDOWS;i++)if(surfaces[i].handle){
        Surface *s=surfaces+i;
        unsigned on=active&&(panel_flags&1)?s->handle==panel_window:s->handle==xd_focus();
        if(s->focused!=on){s->focused=on;zero(s->held,sizeof(s->held));s->buttons=0;
            desktop_event(s,6,on?8:0,0,0);desktop_changed();}
    }
}
static void desktop_invalidate_surface(Surface *s) {
    if(s->handle==panel_window)xd_invalidate(0,0,platform.width,platform.height);
    else for(unsigned i=0;i<WINDOWS;i++){int32_t r[6];if(xd_window(i,r)==s->handle){xd_invalidate(r[0]-2,r[1]-28,r[2]+4,r[3]+30);break;}}
}
static void desktop_panel_release(void) {
    Surface *p=desktop_surface(panel_window);
    if(p){p->flags|=2;p->focused=0;p->buttons=0;zero(p->held,sizeof(p->held));
        if(p->captured){p->captured=0;desktop_event(p,7,0,p->x,p->y);}desktop_event(p,6,0,0,0);}
    panel_window=panel_process=panel_flags=panel_top=panel_bottom=panel_count=panel_capture=0;
    xd_edges(0,34,0);desktop_changed();xd_invalidate(0,0,platform.width,platform.height);
    Surface *focus=desktop_surface(xd_focus());if(focus&&!focus->focused)desktop_action(1,focus->handle,1,0,0,0);
}
static void desktop_destroy(Surface *s) {
    if(s->handle==panel_window)desktop_panel_release();
    else if(s->mapped)xd_remove(s->handle);
    if(s->front)platform.release(s->front,(size_t)s->width*s->height*4);
    s->front=0;s->mapped=0;s->flags|=2;desktop_changed();
    if(desktop_hover==s->handle)desktop_hover=0;
    desktop_compose();
}
static void desktop_owner_release(uint64_t owner) {
    for(unsigned i=0;i<SESSION_PROCESSES;i++)if(desktop_processes[i].id&&desktop_processes[i].owner==owner){
        if(desktop_processes[i].id==panel_process)desktop_panel_release();
        zero(&desktop_processes[i],sizeof(desktop_processes[i]));desktop_changed();
    }
}
static void desktop_publish(Surface *s) {
    if(s->flags&2)return;
    if(!s->front)return;
    int32_t left=s->left,top=s->top,right=s->right,bottom=s->bottom;
    unsigned fresh=!s->mapped;
    for(unsigned y=(unsigned)s->top;y<(unsigned)s->bottom;y++)
        for(unsigned x=(unsigned)s->left;x<(unsigned)s->right;x++)
            s->front[(size_t)y*s->width+x]=s->pixels[(size_t)y*s->width+x];
    s->left=s->width;s->top=s->height;s->right=s->bottom=0;
    if(!s->mapped){s->mapped=1;if(s->handle!=panel_window)xd_add(s->handle,s->width,s->height,!(s->flags&1));desktop_changed();}
    if(s->handle==panel_window)desktop_panel_sync();
    if(fresh)desktop_invalidate_surface(s);
    else if(left<right&&top<bottom){
        if(s->handle==panel_window)xd_invalidate(left,top,right-left,bottom-top);
        else for(unsigned i=0;i<WINDOWS;i++){int32_t r[6];if(xd_window(i,r)==s->handle){
            int32_t x=r[0]+(int32_t)((int64_t)left*r[2]/s->width),y=r[1]+(int32_t)((int64_t)top*r[3]/s->height);
            int32_t x1=r[0]+(int32_t)(((int64_t)right*r[2]+s->width-1)/s->width),y1=r[1]+(int32_t)(((int64_t)bottom*r[3]+s->height-1)/s->height);
            xd_invalidate(x,y,x1-x,y1-y);break;}}
    }
    desktop_compose();
}
static void desktop_damage(Surface *s,unsigned x,unsigned y,unsigned w,unsigned h) {
    if(x<(unsigned)s->left)s->left=x;if(y<(unsigned)s->top)s->top=y;
    if(x+w>(unsigned)s->right)s->right=x+w;if(y+h>(unsigned)s->bottom)s->bottom=y+h;
}
static uint32_t desktop_over(uint32_t source,uint32_t destination) {
    unsigned a=source>>24;if(!a)return destination;if(a==255)return source;
    uint32_t out=0xff000000;
    for(unsigned shift=0;shift<24;shift+=8)out|=((((source>>shift)&255)*a+((destination>>shift)&255)*(255-a)+127)/255)<<shift;
    return out;
}
static void desktop_compose(void) {
    if(!desktop_active()){
        if(desktop_pixels){platform.release(desktop_pixels,(size_t)platform.width*platform.height*4);desktop_pixels=0;if(platform.restore_console)platform.restore_console();}
        return;
    }
    if(!desktop_pixels){desktop_pixels=platform.allocate((size_t)platform.width*platform.height*4);if(!desktop_pixels)return;xd_invalidate(0,0,platform.width,platform.height);}
    int32_t damage[128];unsigned count=xd_damage(damage);if(!count)return;
    for(unsigned n=0;n<count;n++){
        int32_t *r=damage+n*4;
        int32_t l=r[0]<0?0:r[0],t=r[1]<0?0:r[1],right=r[0]+r[2],bottom=r[1]+r[3];
        if(right>(int32_t)platform.width)right=platform.width;if(bottom>(int32_t)platform.height)bottom=platform.height;
        if(l>=right||t>=bottom)continue;
        for(int32_t y=t;y<bottom;y++)for(int32_t x=l;x<right;x++)desktop_pixels[(size_t)y*platform.width+x]=0xff15191c;
        unsigned normal_count=0;
        for(unsigned i=0;i<WINDOWS;i++){
            int32_t geometry[6];uint32_t id=xd_window(i,geometry);if(!id)break;
            Surface *s=desktop_surface(id);if(!s||!s->front)continue;
            unsigned task=normal_count++;
            if(!(geometry[4]&2)){
                xd_chrome(desktop_pixels,platform.width,platform.height,id,(const uint8_t *)s->title,text_length(s->title),r);
                int32_t x0=geometry[0]>l?geometry[0]:l,y0=geometry[1]>t?geometry[1]:t;
                int32_t x1=geometry[0]+geometry[2],y1=geometry[1]+geometry[3];if(x1>right)x1=right;if(y1>bottom)y1=bottom;
                for(int32_t y=y0;y<y1;y++)for(int32_t x=x0;x<x1;x++){
                    if(geometry[2]==(int32_t)s->width&&geometry[3]==(int32_t)s->height){
                        desktop_pixels[(size_t)y*platform.width+x]=s->front[(size_t)(y-geometry[1])*s->width+x-geometry[0]];continue;}
                    unsigned sx=(unsigned)((uint64_t)(x-geometry[0])*s->width/(unsigned)geometry[2]);
                    unsigned sy=(unsigned)((uint64_t)(y-geometry[1])*s->height/(unsigned)geometry[3]);
                    desktop_pixels[(size_t)y*platform.width+x]=s->front[(size_t)sy*s->width+sx];
                }
            }
            if(!desktop_panel_active()){
                int32_t tr[4];xd_task(task,tr);int32_t tx=tr[0],ty=tr[1];
                for(int32_t y=ty;y<ty+tr[3];y++)for(int32_t x=tx;x<tx+tr[2];x++)
                    if(x>=l&&x<right&&y>=t&&y<bottom)desktop_pixels[(size_t)y*platform.width+x]=(geometry[4]&8)?0xff354355:0xff282c32;
                xd_label(desktop_pixels,platform.width,platform.height,tx+6,ty+2,tr[2]-12,tr[3]-4,(const uint8_t *)s->title,text_length(s->title),r);
            }
        }
        if(desktop_panel_active()){
            Surface *s=desktop_surface(panel_window);
            for(int32_t y=t;y<bottom;y++)for(int32_t x=l;x<right;x++){
                int inside=0;for(unsigned j=0;j<panel_count;j++){PanelRect *p=panel_regions+j;if(x>=(int32_t)p->x&&y>=(int32_t)p->y&&x<(int32_t)(p->x+p->w)&&y<(int32_t)(p->y+p->h)){inside=1;break;}}
                if(inside){size_t at=(size_t)y*platform.width+x;desktop_pixels[at]=(panel_flags&4)?desktop_over(s->front[at],desktop_pixels[at]):s->front[at];}
            }
        }
    }
    if(platform.present)platform.present(desktop_pixels,platform.width,platform.height);
}
static void desktop_key(unsigned usage,int down) {
    if(usage>=256)return;unsigned bit=1U<<(usage&15);
    if(down)desktop_keys[usage/16]|=bit;else desktop_keys[usage/16]&=~bit;
    Surface *s=desktop_surface(desktop_panel_active()&&(panel_flags&1)?panel_window:xd_focus());if(!s)return;
    if(down)s->held[usage/16]|=bit;else s->held[usage/16]&=~bit;
    if(down&&usage==61&&(desktop_keys[14]&0x44)){s->closing=1;desktop_event(s,8,0,0,0);desktop_changed();return;}
    enqueue(s,(Event){.type=1,.flags=down?1:0,.code=usage});
}
static void desktop_pointer(int32_t x,int32_t y,uint32_t buttons,int32_t wheel) {
    buttons&=7;
    unsigned panel=(panel_capture||desktop_panel_hit(x,y))&&!xd_captured();
    uint32_t hover=panel?panel_window:xd_hit(x,y);
    if(desktop_hover!=hover){Surface *old=desktop_surface(desktop_hover),*next=desktop_surface(hover);
        if(old)desktop_event(old,5,0,old->x,old->y);if(next)desktop_event(next,5,4,x,y);desktop_hover=hover;}
    if(panel){Surface *s=desktop_surface(panel_window);s->x=x;s->y=y;desktop_event(s,2,0,x,y);
        unsigned changed=desktop_buttons^buttons;s->buttons=buttons;
        if(buttons&&!panel_capture){panel_capture=1;s->captured=1;desktop_event(s,7,2,x,y);}
        for(unsigned i=0;i<3;i++)if(changed&(1U<<i))enqueue(s,(Event){.type=3,.flags=buttons&(1U<<i)?1:0,.code=i+1,.buttons=buttons,.x=x,.y=y});
        if(wheel)enqueue(s,(Event){.type=4,.buttons=buttons,.x=x,.y=y,.wy=wheel});
        if(!buttons&&panel_capture){panel_capture=0;s->captured=0;desktop_event(s,7,0,x,y);}
    }else{
        xd_motion(x,y);unsigned changed=desktop_buttons^buttons;
        for(unsigned i=0;i<3;i++)if(changed&(1U<<i))xd_button(i,(buttons>>i)&1);
        Surface *s=desktop_surface(xd_hit(x,y));if(s&&wheel)enqueue(s,(Event){.type=4,.buttons=buttons,.x=s->x,.y=s->y,.wy=wheel});
    }
    desktop_buttons=buttons;desktop_compose();
}
static void record_text(uint16_t *record,unsigned offset,const char *text,unsigned limit){unsigned n=text_length(text);if(n>=limit)n=limit-1;for(unsigned i=0;i<n;i++)record[offset+i]=(unsigned char)text[i];}
static void process_record(uint16_t *record,DesktopProcess *p,unsigned instance) {
    if(!p)return;
    if(instance){word(record,0,0);word(record,2,desktop_process(p->leader)?p->leader:0);word(record,4,p->instance);DesktopProcess *leader=desktop_process(p->leader);record[8]=leader?leader->pid:0;}
    else {word(record,2,p->id);word(record,4,p->instance);record[8]=p->pid;}
    record_text(record,18,p->app,64);record_text(record,82,p->name,64);record_text(record,146,p->icon,128);
}
static void window_record(uint16_t *record,Surface *s) {
    word(record,0,s->handle);word(record,2,s->process);word(record,6,1);record[8]=s->pid;
    DesktopProcess *p=desktop_process(s->process);if(p){word(record,4,p->instance);record_text(record,18,p->app,64);record_text(record,82,p->name,64);record_text(record,146,p->icon,128);}
    unsigned flags=(s->flags&2)?32:s->mapped?1:32;
    if(s->focused)flags|=8;if(!(s->flags&1))flags|=16;if(s->closing)flags|=64;
    if(s->handle==panel_window){record[10]=1;record[16]=s->width;record[17]=s->height;}
    else {for(unsigned i=0;i<WINDOWS;i++){int32_t r[6];if(xd_window(i,r)==s->handle){flags=(flags&~7U)|((unsigned)r[4]&7);word(record,12,r[0]);word(record,14,r[1]);record[16]=r[2];record[17]=r[3];break;}}}
    if(!record[16]){record[16]=s->width;record[17]=s->height;}
    record[9]=flags;record_text(record,274,s->title,64);
}
static int32_t desktop_session(uint64_t owner,uint32_t a[12],const GxGuest *g) {
    unsigned op=a[0];uint16_t *out=0;uint32_t expected=a[4];
    if(op==1||op==5||op==6||op==7||op==8){out=span(g,a[6],338,1);if(!out)return EFAULT_;}
    if(expected&&op>=5&&op<=8&&expected!=desktop_revision)return -11;
    a[7]=0;
    if(op==64){if(!desktop_touch(a[1],a[2],owner))return ENOMEM_;}
    else if(op==65){
        if(panel_process==a[1])desktop_panel_release();DesktopProcess *p=desktop_process(a[1]);if(p){zero(p,sizeof(*p));desktop_changed();}
    }else if(op==1){
        char app[64],name[64],icon[128];int error;
        if((error=desktop_text_cells(out+18,app,64))||(error=desktop_text_cells(out+82,name,64))||(error=desktop_text_cells(out+146,icon,128)))return error;
        if(!app[0])return EINVAL_;for(unsigned i=0;app[i];i++){unsigned c=(unsigned char)app[i];if(!((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='.'||c=='_'||c=='-'))return EINVAL_;}
        DesktopProcess *p=desktop_touch(a[1],a[2],owner);if(!p)return ENOMEM_;
        if(p->instance&&!text_equal(p->app,app))return EINVAL_;
        if(p->instance&&p->leader!=p->id&&(!text_equal(p->name,name)||!text_equal(p->icon,icon)))return EINVAL_;
        if(!p->instance){if(desktop_instance==UINT32_MAX)return ENOMEM_;p->instance=++desktop_instance;p->leader=p->id;}
        if(!text_equal(p->name,name)||!text_equal(p->icon,icon)||!text_equal(p->app,app)){
            text_copy(p->app,app,64);text_copy(p->name,name,64);text_copy(p->icon,icon,128);desktop_changed();}
        zero(out,338*2);process_record(out,p,0);a[3]=p->id;a[5]=p->instance;
    }else if(op==2){
        DesktopProcess *parent=desktop_process(a[1]);if(!parent||!parent->instance)return EINVAL_;
        DesktopProcess *child=desktop_touch(a[8],a[9],0);if(!child)return ENOMEM_;
        if(child->instance&&child->instance!=parent->instance)return EINVAL_;
        if(child->instance!=parent->instance){child->instance=parent->instance;child->leader=parent->leader;
            text_copy(child->app,parent->app,64);text_copy(child->name,parent->name,64);text_copy(child->icon,parent->icon,128);desktop_changed();}
        a[3]=child->id;a[5]=child->instance;
    }else if(op==5||op==6){
        Surface *s=0;uint32_t next=UINT32_MAX;
        for(unsigned i=0;i<WINDOWS;i++)if(surfaces[i].handle&&((op==6&&surfaces[i].handle==a[3])||(op==5&&surfaces[i].handle>a[3]&&surfaces[i].handle<next))){s=&surfaces[i];next=s->handle;}
        if(op==6&&!s)return -2;zero(out,338*2);a[3]=s?s->handle:0;if(s)window_record(out,s);
    }else if(op==7||op==8){
        DesktopProcess *p=0;uint32_t next=UINT32_MAX;
        for(unsigned i=0;i<SESSION_PROCESSES;i++){DesktopProcess *q=desktop_processes+i;uint32_t id=op==7?q->instance:q->id;if(id>a[3]&&id<next){p=q;next=id;}}
        zero(out,338*2);a[3]=p?next:0;if(p)process_record(out,p,op==7);
    }else if(op==9){
        int32_t error;Surface *s=desktop_surface(a[3]);if(!s)return -2;if(s->handle==panel_window)return EOPNOTSUPP_;
        int32_t x=0,y=0;unsigned w=0,h=0;
        if(a[5]==5||a[5]==6){uint16_t *r=span(g,a[6],338,0);if(!r)return EFAULT_;x=(int32_t)(r[12]|(uint32_t)r[13]<<16);y=(int32_t)(r[14]|(uint32_t)r[15]<<16);w=r[16];h=r[17];}
        error=xd_command(s->handle,a[5],x,y,w,h);if(error)return error;desktop_changed();desktop_compose();
    }else if(op==10){if(expected>desktop_revision)return EINVAL_;a[7]=expected!=desktop_revision;}
    else if(op!=0&&op!=66&&op!=64&&op!=65)return EOPNOTSUPP_;
    if(op==0||op>=64){unsigned windows=0,instances=0;for(unsigned i=0;i<WINDOWS;i++)windows+=surfaces[i].handle!=0;
        for(unsigned i=0;i<SESSION_PROCESSES;i++)if(desktop_processes[i].instance){int first=1;for(unsigned j=0;j<i;j++)if(desktop_processes[j].instance==desktop_processes[i].instance)first=0;instances+=first;}
        a[3]=windows;a[5]=instances;a[7]=15;}
    a[4]=desktop_revision;return 0;
}
static int32_t desktop_panel(uint64_t owner,uint32_t a[12],const GxGuest *g) {
    if(a[0]==0){a[3]=12;a[8]=platform.width;a[9]=platform.height;return 0;}
    if(a[0]==64){if(panel_process==a[1]){desktop_panel_release();desktop_compose();}return 0;}
    if(a[0]==2&&!a[2]){if(panel_process!=a[1])return EPERM_;desktop_panel_release();desktop_compose();return 0;}
    int32_t error=0;Surface *s=owned(owner,a[2],&error);if(!s)return error;
    if(s->process!=a[1])return EPERM_;
    if(a[0]==2){if(panel_window!=s->handle)return EINVAL_;desktop_panel_release();desktop_compose();return 0;}
    if(a[0]==1){
        unsigned flags=a[3],count=a[4],top=a[6],bottom=a[7];
        if((flags&~7U)||((flags&2)&&!(flags&1))||count>PANEL_REGIONS||top+bottom+78>platform.height||s->width!=platform.width||s->height!=platform.height)return EINVAL_;
        if(!count&&(a[5]||(flags&~4U)||top||bottom))return EINVAL_;
        if(panel_window&&panel_window!=s->handle)return -16;
        if((flags&4)&&s->format!=2)return EINVAL_;
        uint16_t *rects=count?span(g,a[5],(size_t)count*4,0):0;if(count&&!rects)return EFAULT_;
        for(unsigned i=0;i<count;i++){uint16_t *r=rects+i*4;if(!r[2]||!r[3]||r[0]>platform.width||r[2]>platform.width-r[0]||r[1]>platform.height||r[3]>platform.height-r[1])return EINVAL_;}
        unsigned same=panel_window==s->handle&&panel_process==s->process&&panel_flags==flags&&panel_count==count&&panel_top==top&&panel_bottom==bottom;
        for(unsigned i=0;same&&i<count;i++){PanelRect *p=panel_regions+i;uint16_t *r=rects+i*4;
            if(p->x!=r[0]||p->y!=r[1]||p->w!=r[2]||p->h!=r[3])same=0;}
        if(same){a[8]=platform.width;a[9]=platform.height;return 0;}
        if(s->mapped&&s->handle!=panel_window)xd_remove(s->handle);
        if((flags&4)&&!(panel_flags&4)){zero(s->pixels,(size_t)s->width*s->height*4);zero(s->front,(size_t)s->width*s->height*4);desktop_damage(s,0,0,s->width,s->height);}
        panel_window=s->handle;panel_process=s->process;panel_flags=flags;panel_top=top;panel_bottom=bottom;panel_count=count;
        for(unsigned i=0;i<count;i++)panel_regions[i]=(PanelRect){rects[i*4],rects[i*4+1],rects[i*4+2],rects[i*4+3]};
        desktop_panel_sync();
        xd_invalidate(0,0,platform.width,platform.height);desktop_changed();desktop_compose();a[8]=platform.width;a[9]=platform.height;return 0;
    }
    if(a[0]==3){
        unsigned x=a[3],y=a[4],w=a[5],h=a[6],stride=a[7];
        if(panel_window!=s->handle||!(panel_flags&4)||!w||!h||!rectangle(s,x,y,w,h)||stride<(size_t)w*4)return EINVAL_;
        uint16_t *data=span(g,a[8],(size_t)(h-1)*stride+(size_t)w*4,0);if(!data)return EFAULT_;
        for(unsigned j=0;j<h;j++)for(unsigned i=0;i<w;i++){size_t at=(size_t)j*stride+i*4;s->pixels[(size_t)(y+j)*s->width+x+i]=(data[at]&255)|(data[at+1]&255)<<8|(data[at+2]&255)<<16|(uint32_t)(data[at+3]&255)<<24;}
        desktop_damage(s,x,y,w,h);return 0;
    }return EOPNOTSUPP_;
}
static int32_t desktop_configure(Surface *s,unsigned w,unsigned h,unsigned flags) {
    if(!w||!h||w>MAX_WIDTH||h>MAX_HEIGHT||(flags&~3U))return EINVAL_;
    if(w!=s->width||h!=s->height){
        uint32_t *back=platform.allocate((size_t)w*h*4),*front=platform.allocate((size_t)w*h*4);
        if(!back||!front){if(back)platform.release(back,(size_t)w*h*4);if(front)platform.release(front,(size_t)w*h*4);return ENOMEM_;}
        zero(back,(size_t)w*h*4);zero(front,(size_t)w*h*4);
        for(unsigned y=0;y<h&&y<s->height;y++)for(unsigned x=0;x<w&&x<s->width;x++){back[(size_t)y*w+x]=s->pixels[(size_t)y*s->width+x];front[(size_t)y*w+x]=s->front[(size_t)y*s->width+x];}
        platform.release(s->pixels,(size_t)s->width*s->height*4);platform.release(s->front,(size_t)s->width*s->height*4);
        s->pixels=back;s->front=front;s->width=w;s->height=h;
    }
    unsigned changed=s->flags!=flags||s->pending_width||s->pending_height;
    if((flags&2)&&s->mapped&&s->handle!=panel_window){xd_remove(s->handle);s->mapped=0;}
    s->flags=flags;s->pending_width=s->pending_height=0;s->left=s->top=0;s->right=w;s->bottom=h;
    if(s->handle==panel_window)desktop_panel_sync();
    if(s->mapped&&s->handle!=panel_window)xd_accept(s->handle,w,h,!(flags&1));
    if(changed){desktop_changed();desktop_invalidate_surface(s);desktop_compose();}return 0;
}
static int32_t desktop_call(uint64_t owner,unsigned op,uint32_t a[12],const GxGuest *g) {
    if(!owner)return EPERM_;
    if(op==0){a[0]=1|2|4|8|16|32|64|128|256|512|1024|4096|8192|16384|32768U;a[1]=MAX_WIDTH;a[2]=MAX_HEIGHT;a[3]=WINDOWS;return 0;}
    if(op==31)return desktop_session(owner,a,g);
    if(op==32)return desktop_panel(owner,a,g);
    int32_t error=0;Surface *s=op==1?0:desktop_surface(a[0]);
    if(op==12){s=owned(owner,a[0],&error);return s?desktop_configure(s,a[1],a[2],a[3]):error;}
    if(op==7){s=owned(owner,a[0],&error);if(!s)return error;char title[64];error=desktop_text(g,a[1],title,64);if(error)return error;
        if(!text_equal(s->title,title)){text_copy(s->title,title,64);desktop_changed();desktop_invalidate_surface(s);desktop_compose();}return 0;}
    if(op==13){s=owned(owner,a[0],&error);if(!s)return error;if(!s->mapped)desktop_publish(s);else if(s->handle!=panel_window){error=xd_command(s->handle,1,0,0,0,0);desktop_changed();desktop_compose();}return error;}
    if(op==9){s=owned(owner,a[0],&error);if(!s)return error;uint16_t *state=span(g,a[1],25,1);if(!state)return EFAULT_;
        zero(state,50);state[0]=1;word(state,1,s->handle);state[3]=(s->focused?8:0)|(s->captured?2:0);state[4]=s->buttons;for(unsigned i=0;i<16;i++)state[5+i]=s->held[i];word(state,21,s->x);word(state,23,s->y);return 0;}
    if(op==19||op==20){s=owned(owner,a[0],&error);if(!s)return error;unsigned n=op==19?desktop_clipboard_length:a[2];
        if(a[2]>32768||a[3]>32768)return EINVAL_;if(op==19&&n>a[3])return -34;
        uint16_t *data=n?span(g,a[1],n,op==19):0;if(n&&!data)return EFAULT_;
        if(op==20){if(!utf8_valid(data,n))return EINVAL_;for(unsigned i=0;i<n;i++)if(!data[i]||data[i]>255)return EINVAL_;
            for(unsigned i=0;i<n;i++)desktop_clipboard[i]=data[i];desktop_clipboard_length=n;
        }else{for(unsigned i=0;i<n;i++)data[i]=desktop_clipboard[i];a[2]=n;}return 0;}
    int32_t result=gx_basic_call(owner,op,a,g);if(result)return result;
    if(op==1){s=desktop_surface(a[0]);s->front=platform.allocate((size_t)s->width*s->height*4);
        if(!s->front){destroy(s);return ENOMEM_;}zero(s->front,(size_t)s->width*s->height*4);
        s->process=a[5];s->pid=a[6];s->left=s->top=0;s->right=s->width;s->bottom=s->height;
        if(desktop_text(g,a[2],s->title,64)){destroy(s);return EINVAL_;}
        if(s->process&&!desktop_touch(s->process,s->pid,owner)){destroy(s);return ENOMEM_;}desktop_changed();
    }else if(s&&(op==3||op==5||op==6||op==17||op==18||op==23||op==27)){
        if(op==6)desktop_damage(s,a[5],a[6],a[3],a[4]);
        else if(op==17||op==18)desktop_damage(s,0,0,s->width,s->height);
        else desktop_damage(s,a[1],a[2],a[3],a[4]);
    }
    return 0;
}
