/* Exercise production native desktop policy, transport and publication together. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define BOOT_DESKTOP 1
#include "../boot/gx.c"
static uint16_t cells[80000];
static uint32_t frame[800*600];
static unsigned allocated,released,presents,restores;
/* Unwind is disabled; the host's precompiled core archive still names it. */
void rust_eh_personality(void){abort();}
void unsupported_service(const unsigned char *text){fprintf(stderr,"native fatal: %s\n",text);abort();}
static void *allocate_test(size_t n){++allocated;return calloc(1,n);}
static void release_test(void *p,size_t n){(void)n;++released;free(p);}
static void present_test(const uint32_t *p,unsigned w,unsigned h){assert(w==800&&h==600);++presents;memcpy(frame,p,w*h*4);}
static void restore_test(void){++restores;}
static uint16_t *span_test(void *p,uint32_t a,size_t n,int write){(void)p;(void)write;return a>=32&&a<80000&&n<=80000-a?cells+a:NULL;}
static GxGuest guest={NULL,span_test};
static void put_text(unsigned at,const char *s){while(*s)cells[at++]=(unsigned char)*s++;cells[at]=0;}
static int request(uint64_t owner,unsigned op,uint32_t a[12]){return gx_call(owner,op,a,&guest);}
static uint32_t create_test(uint64_t owner,unsigned process,unsigned w,unsigned h,unsigned hidden){
    put_text(32,"Test window");uint32_t a[12]={w,h,32,hidden?2:0,2,process,(unsigned)owner};
    assert(!request(owner,1,a));return a[0];
}
static void registration(unsigned owner,unsigned process,const char *app){
    memset(cells+2000,0,338*2);put_text(2018,app);put_text(2082,app);put_text(2146,"calculator");
    uint32_t a[12]={1,process,owner,0,0,0,2000};assert(!request(owner,31,a));assert(a[3]==process&&a[5]);
}
static void fill_test(unsigned owner,uint32_t window,uint32_t rgb){
    Surface *s=desktop_surface(window);uint32_t a[12]={window,0,0,s->width,s->height,rgb};assert(!request(owner,27,a));
}
static void present_window(unsigned owner,uint32_t window){uint32_t a[12]={window};assert(!request(owner,4,a));}
static void control(unsigned process,uint32_t window,unsigned command,int x,int y){
    memset(cells+2000,0,338*2);word(cells+2000,12,(uint32_t)x);word(cells+2000,14,(uint32_t)y);
    uint32_t a[12]={9,process,process,window,0,command,command==5?2000:0};assert(!request(process,31,a));
}
int main(void){
    GxPlatform p={800,600,allocate_test,release_test,present_test,restore_test};gx_bind(&p);
    registration(1,101,"org.riplix.Calculator");registration(2,102,"org.riplix.TextEditor");
    uint32_t first=create_test(1,101,260,120,0),second=create_test(2,102,260,120,0);
    fill_test(1,first,0x123456);assert(!presents);present_window(1,first);
    fill_test(2,second,0xabcdef);present_window(2,second);assert(xd_focus()==second);
    control(2,second,5,430,150);
    int32_t geometry[6];assert(xd_window(0,geometry)==first);
    unsigned at=(unsigned)(geometry[1]+40)*800+(unsigned)geometry[0]+40;
    assert((frame[at]&0xffffff)==0x123456);
    /* Background publication neither changes focus nor exposes another app's
     * uncommitted back buffer when a desktop move triggers composition. */
    fill_test(1,first,0x112233);control(2,second,5,450,180);assert((frame[at]&0xffffff)==0x123456);
    present_window(1,first);assert(xd_focus()==second&&(frame[at]&0xffffff)==0x112233);
    unsigned before=presents;uint32_t bad[12]={first,250,0,20,20,0};assert(request(1,27,bad)==EINVAL_&&presents==before);
    uint32_t wrong[12]={first};assert(request(2,4,wrong)==EPERM_);
    /* Snapshots detect changes and do not advance for ordinary repaints. */
    uint32_t q[12]={0};assert(!request(1,31,q));unsigned revision=q[4];
    present_window(1,first);memset(q,0,sizeof(q));assert(!request(1,31,q)&&q[4]==revision);
    control(2,second,2,0,0);assert(xd_focus()==first);
    uint32_t stale[12]={5,101,1,0,revision,0,2000};assert(request(1,31,stale)==-11);
    control(2,second,1,0,0);assert(xd_focus()==second);
    gx_key(4,1,0);control(1,first,1,0,0);assert(!desktop_surface(second)->held[0]);gx_key(4,0,0);
    gx_pointer(geometry[0]+30,geometry[1]+30,1,0);assert(xd_captured());gx_pointer(-20,-20,0,0);assert(!xd_captured());
    /* Full-output panel, atomic alpha publication, transparent hit regions,
     * modal dismissal and manager revocation restoring fallback controls. */
    registration(3,103,"org.riplix.FuseDE");uint32_t panel=create_test(3,103,800,600,1);
    uint32_t config[12]={1,103,panel,4,0,0,0,0};assert(!request(3,32,config));
    uint32_t show[12]={panel,800,600,0};assert(!request(3,12,show));
    cells[1000]=700;cells[1001]=550;cells[1002]=80;cells[1003]=30;
    config[3]=4;config[4]=1;config[5]=1000;config[7]=40;assert(!request(3,32,config));
    cells[1100]=0;cells[1101]=0;cells[1102]=255;cells[1103]=128;
    uint32_t upload[12]={3,103,panel,700,550,1,1,4,1100};assert(!request(3,32,upload));
    unsigned old=frame[550*800+700];assert(old!=desktop_over(0x80ff0000,old));present_window(3,panel);
    assert(frame[550*800+700]==desktop_over(0x80ff0000,old));
    assert(desktop_panel_hit(700,550)&&!desktop_panel_hit(701,550));
    gx_pointer(700,550,1,0);assert(panel_capture&&desktop_surface(panel)->captured);
    gx_pointer(200,200,0,0);assert(!panel_capture&&!desktop_surface(panel)->captured&&!desktop_surface(panel)->buttons);
    config[3]=7;assert(!request(3,32,config));assert(desktop_panel_hit(10,10));
    unsigned revision_before=desktop_revision,presents_before=presents;
    assert(!request(3,32,config)&&desktop_revision==revision_before&&presents==presents_before);
    gx_pointer(10,10,1,0);gx_pointer(10,10,0,0);assert(desktop_surface(panel)->count);
    show[3]=2;assert(!request(3,12,show));assert(!desktop_panel_active()&&!desktop_panel_hit(10,10));
    assert(desktop_surface(xd_focus())->focused&&!desktop_surface(panel)->focused);
    control(2,second,1,0,0);assert(desktop_surface(second)->focused);
    show[3]=0;assert(!request(3,12,show));assert(desktop_panel_active()&&desktop_surface(panel)->focused);
    uint32_t revoke[12]={64,103};assert(!request(3,32,revoke)&&!panel_window);
    gx_release(3);gx_release(2);assert(desktop_surface(first));gx_release(1);
    assert(!gx_active()&&allocated==released&&restores);
    puts("NATIVE_DESKTOP_OK: retained publication, independent focus, ownership, snapshots, input capture, alpha panels, revocation, balanced cleanup");
}
