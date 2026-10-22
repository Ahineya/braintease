/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Real guest-process acceptance checks for the optional desktop session ABI. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <stdint.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/wait.h>
#include <gx.h>
#include <gx-session.h>

#define C(x) do { if (!(x)) { fprintf(stderr,"SESSION_FAILED line %u errno %d: %s\n",__LINE__,errno,#x); exit(1); } } while (0)
static int gfx;
static unsigned checks;
struct reply { uint32_t window,process,instance,value; };

static void exact(int fd,void *buffer,unsigned size,int writing) {
    char *p=buffer;
    while(size) { int n=writing?write(fd,p,size):read(fd,p,size); C(n>0);p+=n;size-=n; }
}
static void passed(const char *name) { ++checks;printf("SESSION_CHECK %s\n",name);fflush(stdout); }
static void init(struct gx_session *s,unsigned operation,struct gx_session_record *r) {
    memset(s,0,sizeof(*s));s->version=GXS_VERSION;s->operation=operation;s->record=r;
}
static struct gx_session information(void) {
    struct gx_session s;init(&s,GXS_INFO,NULL);C(ioctl(gfx,GX_SESSION,&s)==0);return s;
}
static struct gx_session_record registration(const char *id,const char *name) {
    struct gx_session s;struct gx_session_record r;memset(&r,0,sizeof(r));
    strcpy(r.app_id,id);strcpy(r.name,name);strcpy(r.icon,"/usr/share/icons/session-probe.png");
    init(&s,GXS_REGISTER,&r);C(ioctl(gfx,GX_SESSION,&s)==0);C(r.process&&r.instance);return r;
}
static struct gx_session_record get(uint32_t id) {
    struct gx_session s;struct gx_session_record r;memset(&r,0,sizeof(r));
    init(&s,GXS_GET_WINDOW,&r);s.object=id;C(ioctl(gfx,GX_SESSION,&s)==0);C(r.window==id);return r;
}
static uint32_t find_pid(unsigned pid,unsigned operation) {
    struct gx_session s;struct gx_session_record r;uint32_t found=0,previous=0;
    init(&s,operation,&r);
    for(;;) { C(ioctl(gfx,GX_SESSION,&s)==0);if(!s.object)break;C(s.object>previous);previous=s.object;
        C(s.object==(operation==GXS_NEXT_PROCESS?r.process:r.window));if(r.pid==pid){C(!found);found=s.object;} }
    C(found);return found;
}
static void absent(uint32_t id,unsigned operation) {
    struct gx_session s;struct gx_session_record r;uint32_t previous=0;
    init(&s,operation,&r);
    for(;;) { C(ioctl(gfx,GX_SESSION,&s)==0);if(!s.object)break;C(s.object>previous&&s.object!=id);previous=s.object; }
}
static struct gx_window window(const char *name) {
    struct gx_window w;struct gx_handle h;memset(&w,0,sizeof(w));
    w.version=GX_VERSION;w.width=160;w.height=100;w.format=GX_RGB565;strcpy(w.title,name);
    C(ioctl(gfx,GX_CREATE,&w)==0);h.version=GX_VERSION;h.handle=w.window;
    C(ioctl(gfx,GX_PRESENT,&h)==0);return w;
}
static void drain(uint32_t id) {
    struct gx_event e;unsigned limit=0;
    do { memset(&e,0,sizeof(e));e.version=GX_VERSION;e.window=id;C(ioctl(gfx,GX_EVENT,&e)==0);C(++limit<200); }while(e.type);
}
static int ready(unsigned milliseconds) {
    fd_set fds;struct timeval timeout;
    FD_ZERO(&fds);FD_SET(gfx,&fds);timeout.tv_sec=milliseconds/1000;timeout.tv_usec=(milliseconds%1000)*1000L;
    return select(gfx+1,&fds,NULL,NULL,&timeout);
}
static uint32_t watch(uint32_t revision,unsigned changed) {
    struct gx_session s;init(&s,GXS_WATCH,NULL);s.revision=revision;C(ioctl(gfx,GX_SESSION,&s)==0);
    C(!!(s.flags&GXS_CHANGED)==!!changed);return s.revision;
}
static void control(uint32_t id,uint32_t token,unsigned command,int32_t x,int32_t y,unsigned width,unsigned height) {
    struct gx_session s;struct gx_session_record r;memset(&r,0,sizeof(r));r.x=x;r.y=y;r.width=width;r.height=height;
    init(&s,GXS_CONTROL,command==GXS_MOVE||command==GXS_RESIZE?&r:NULL);s.authority=token;s.object=id;s.value=command;C(ioctl(gfx,GX_SESSION,&s)==0);
}
static struct reply command(int out,int in,char c) {
    struct reply r;exact(out,&c,1,1);exact(in,&r,sizeof(r),0);return r;
}
static int child(int input,int output,unsigned helper) {
    struct gx_session_record metadata;struct gx_window w;struct reply response;char c;
    struct gx_session s;struct gx_handle h;struct gx_title title;unsigned titles=0;
    gfx=open("/dev/gfx",O_RDWR);C(gfx>=0);memset(&metadata,0,sizeof(metadata));
    if(!helper)metadata=registration("org.riplix.session-child","Session child");
    w=window("Session child");memset(&response,0,sizeof(response));response.window=find_pid(getpid(),GXS_NEXT_WINDOW);
    response.process=metadata.process;response.instance=metadata.instance;exact(output,&response,sizeof(response),1);
    while(read(input,&c,1)==1) {
        response.value=0;
        if(c=='t') { memset(&title,0,sizeof(title));title.version=GX_VERSION;title.window=w.window;strcpy(title.title,titles++?"Session renamed child again":"Session renamed child");C(ioctl(gfx,GX_TITLE,&title)==0); }
        else if(c=='e') {
            struct gx_event e;
            do { memset(&e,0,sizeof(e));e.version=GX_VERSION;e.window=w.window;C(ioctl(gfx,GX_EVENT,&e)==0);
                if(e.type==GX_RESIZE) { struct gx_configure configure;memset(&configure,0,sizeof(configure));configure.version=GX_VERSION;configure.window=w.window;configure.width=e.x;configure.height=e.y;C(ioctl(gfx,GX_CONFIGURE,&configure)==0); }
                if(e.type==GX_CLOSE)response.value|=1;
            }while(e.type);
        }
        else if(c=='d') { h.version=GX_VERSION;h.handle=w.window;C(ioctl(gfx,GX_DESTROY,&h)==0);w.window=0; }
        else if(c=='m') { init(&s,GXS_CLAIM_MANAGER,NULL);C(ioctl(gfx,GX_SESSION,&s)==0);response.value=s.authority;C(response.value); }
        else if(c=='q')break;
        else C(0);
        exact(output,&response,sizeof(response),1);
    }
    close(gfx);return 0;
}

int main(int argc,char **argv) {
    struct gx_session s,info;struct gx_session_record mine,theirs,r;struct gx_window w;
    struct gx_fill fill;struct gx_handle h;struct reply reply;int requests[2],replies[2],pid,status;
    uint32_t own,other,token,revision,old_process,old_instance,old_window,old_own;unsigned nested;
    C(sizeof(struct gx_session_record)==338);
    if(argc==4&&(!strcmp(argv[1],"--child")||!strcmp(argv[1],"--helper")))return child(atoi(argv[2]),atoi(argv[3]),!strcmp(argv[1],"--helper"));
    nested=argc>1&&!strcmp(argv[1],"--nested");gfx=open("/dev/gfx",O_RDWR);C(gfx>=0);
    info=information();C(info.flags&GXS_CAP_REGISTRY);C(info.flags&GXS_CAP_WATCH);C(info.flags&GXS_CAP_ATTACH_CHILD);
    C(!!(info.flags&GXS_CAP_CONTROL)==!!nested);passed("capability discovery and C16 record layout");
    mine=registration("org.riplix.session-probe","Session probe");w=window("Session parent");
    own=find_pid(getpid(),GXS_NEXT_WINDOW);old_own=own;r=get(own);
    C(r.process==mine.process&&r.instance==mine.instance&&r.pid==getpid());
    C(!strcmp(r.app_id,"org.riplix.session-probe")&&!strcmp(r.name,"Session probe")&&!strcmp(r.title,"Session parent"));
    C(!strcmp(r.icon,"/usr/share/icons/session-probe.png"));passed("registered application instance process and window metadata");
    C(pipe(requests)==0&&pipe(replies)==0);pid=fork();C(pid>=0);
    if(!pid) { char a[12],b[12];char *args[]={"session-probe","--child",a,b,NULL};close(requests[1]);close(replies[0]);snprintf(a,sizeof(a),"%d",requests[0]);snprintf(b,sizeof(b),"%d",replies[1]);execv("/bin/session-probe",args);_exit(91); }
    close(requests[0]);close(replies[1]);exact(replies[0],&reply,sizeof(reply),0);other=reply.window;theirs=get(other);
    C(theirs.pid==pid&&theirs.process==reply.process&&theirs.instance==reply.instance);
    C(theirs.process!=mine.process&&theirs.instance!=mine.instance&&other!=own);
    C(find_pid(pid,GXS_NEXT_PROCESS)==theirs.process);old_process=theirs.process;old_instance=theirs.instance;
    passed("independent fork exec process and application identities");
    init(&s,GXS_ATTACH_CHILD,NULL);s.object=getpid();C(ioctl(gfx,GX_SESSION,&s)<0);
    init(&s,GXS_ATTACH_CHILD,NULL);s.object=pid;C(ioctl(gfx,GX_SESSION,&s)<0);r=get(other);
    C(r.process==theirs.process&&r.instance==theirs.instance&&!strcmp(r.app_id,theirs.app_id));
    passed("parent cannot steal a separately registered child application");
    init(&s,GXS_NEXT_INSTANCE,&r);C(ioctl(gfx,GX_SESSION,&s)==0&&s.object);C(r.instance==s.object);
    init(&s,GXS_NEXT_WINDOW,&r);C(ioctl(gfx,GX_SESSION,&s)==0&&s.object);revision=s.revision;
    command(requests[1],replies[0],'t');errno=0;C(ioctl(gfx,GX_SESSION,&s)<0&&errno==EAGAIN);
    C(!strcmp(get(other).title,"Session renamed child"));passed("snapshot revision restart after title mutation");
    info=information();revision=info.revision;memset(&fill,0,sizeof(fill));fill.version=GX_VERSION;fill.surface=w.surface;fill.width=160;fill.height=100;fill.colour=0x2431;
    C(ioctl(gfx,GX_FILL,&fill)==0);h.version=GX_VERSION;h.handle=w.window;C(ioctl(gfx,GX_PRESENT,&h)==0);
    C(information().revision==revision);passed("drawing and presentation do not mutate registry revision");
    drain(w.window);revision=information().revision;
    init(&s,GXS_WATCH,NULL);s.revision=revision+1;errno=0;C(ioctl(gfx,GX_SESSION,&s)<0&&errno==EINVAL);
    watch(revision,0);C(ready(0)==0);
    command(requests[1],replies[0],'t');C(ready(1000)>0);
    revision=watch(revision,1);drain(w.window);watch(revision,0);C(ready(0)==0);
    passed("title change wakes watch select and acknowledgement restores idle");
    if(nested) {
        control(own,0,GXS_MINIMIZE,0,0,0,0);C(get(own).flags&GXS_MINIMIZED);C(ready(1000)>0);
        revision=watch(revision,1);drain(w.window);watch(revision,0);C(ready(0)==0);
        control(own,0,GXS_ACTIVATE,0,0,0,0);C(!(get(own).flags&GXS_MINIMIZED));drain(w.window);
        passed("owner controls and coalesced watch select wake acknowledgement and idle");
        init(&s,GXS_CONTROL,NULL);s.object=other;s.value=GXS_MINIMIZE;errno=0;C(ioctl(gfx,GX_SESSION,&s)<0&&errno==EPERM);
        passed("foreign window control requires manager authority");
        init(&s,GXS_CLAIM_MANAGER,NULL);C(ioctl(gfx,GX_SESSION,&s)==0);token=s.authority;C(token);
        control(other,token,GXS_MINIMIZE,0,0,0,0);C(get(other).flags&GXS_MINIMIZED);
        control(other,token,GXS_ACTIVATE,0,0,0,0);C(!(get(other).flags&GXS_MINIMIZED));
        control(other,token,GXS_MOVE,300,170,0,0);r=get(other);C(r.x==300&&r.y==170);
        control(other,token,GXS_RESIZE,0,0,200,130);command(requests[1],replies[0],'e');r=get(other);C(r.width==200&&r.height==130);
        control(other,token,GXS_MAXIMIZE,0,0,0,0);command(requests[1],replies[0],'e');C(get(other).flags&GXS_MAXIMIZED);
        control(other,token,GXS_RESTORE,0,0,0,0);command(requests[1],replies[0],'e');r=get(other);C(!(r.flags&GXS_MAXIMIZED)&&r.width==200&&r.height==130&&r.x==300&&r.y==170);
        control(other,token,GXS_CLOSE,0,0,0,0);C(get(other).flags&GXS_CLOSE_PENDING);reply=command(requests[1],replies[0],'e');C(reply.value&1);
        C(get(other).pid==pid);passed("manager minimize activate move resize maximize restore and cooperative close");
        init(&s,GXS_RELEASE_MANAGER,NULL);s.authority=token;C(ioctl(gfx,GX_SESSION,&s)==0);
    } else {
        init(&s,GXS_CONTROL,NULL);s.object=own;s.value=GXS_MINIMIZE;errno=0;C(ioctl(gfx,GX_SESSION,&s)<0&&errno==EOPNOTSUPP);
        passed("native window mode advertises registry without control");
    }
    old_window=other;command(requests[1],replies[0],'d');init(&s,GXS_GET_WINDOW,&r);s.object=old_window;errno=0;C(ioctl(gfx,GX_SESSION,&s)<0&&errno==ENOENT);
    passed("destroyed session window IDs become stale");
    reply=command(requests[1],replies[0],'m');token=reply.value;C(token);
    init(&s,GXS_CLAIM_MANAGER,NULL);errno=0;C(ioctl(gfx,GX_SESSION,&s)<0&&errno==EBUSY);
    init(&s,GXS_RELEASE_MANAGER,NULL);s.authority=token;errno=0;C(ioctl(gfx,GX_SESSION,&s)<0&&errno==EPERM);
    { char quit='q';exact(requests[1],&quit,1,1); }C(waitpid(pid,&status,0)==pid&&status==0);close(requests[1]);close(replies[0]);
    absent(old_process,GXS_NEXT_PROCESS);absent(old_instance,GXS_NEXT_INSTANCE);
    init(&s,GXS_RELEASE_MANAGER,NULL);s.authority=token;C(ioctl(gfx,GX_SESSION,&s)<0);
    init(&s,GXS_CLAIM_MANAGER,NULL);C(ioctl(gfx,GX_SESSION,&s)==0&&s.authority&&s.authority!=token);token=s.authority;
    init(&s,GXS_RELEASE_MANAGER,NULL);s.authority=token;C(ioctl(gfx,GX_SESSION,&s)==0);
    passed("exclusive manager ownership exit revocation and stale authority rejection");
    C(pipe(requests)==0&&pipe(replies)==0);pid=fork();C(pid>=0);
    if(!pid) { char a[12],b[12];char *args[]={"session-probe","--helper",a,b,NULL};close(requests[1]);close(replies[0]);snprintf(a,sizeof(a),"%d",requests[0]);snprintf(b,sizeof(b),"%d",replies[1]);execv("/bin/session-probe",args);_exit(92); }
    close(requests[0]);close(replies[1]);exact(replies[0],&reply,sizeof(reply),0);other=reply.window;theirs=get(other);
    C(theirs.process!=old_process&&theirs.instance!=old_instance&&other!=old_window);
    init(&s,GXS_ATTACH_CHILD,NULL);s.object=pid;C(ioctl(gfx,GX_SESSION,&s)==0);r=get(other);
    C(r.process==theirs.process&&r.instance==mine.instance&&!strcmp(r.app_id,mine.app_id));
    C(find_pid(pid,GXS_NEXT_PROCESS)==theirs.process);passed("explicit actual exec child association without identity spoofing");
    { char quit='q';exact(requests[1],&quit,1,1); }C(waitpid(pid,&status,0)==pid&&status==0);close(requests[1]);close(replies[0]);
    absent(theirs.process,GXS_NEXT_PROCESS);
    init(&s,GXS_GET_WINDOW,&r);s.object=other;errno=0;C(ioctl(gfx,GX_SESSION,&s)<0&&errno==ENOENT);passed("exit cleanup and generation identities survive process slot reuse");
    pid=fork();C(pid>=0);
    if(!pid) { C(setuid(99)==0);init(&s,GXS_CLAIM_MANAGER,NULL);errno=0;C(ioctl(gfx,GX_SESSION,&s)<0&&errno==EPERM);_exit(0); }
    C(waitpid(pid,&status,0)==pid&&status==0);passed("nonroot process cannot claim manager");
    h.handle=w.window;C(ioctl(gfx,GX_DESTROY,&h)==0);w=window("Session replacement");own=find_pid(getpid(),GXS_NEXT_WINDOW);C(own!=old_window&&own!=old_own);
    h.handle=w.window;C(ioctl(gfx,GX_DESTROY,&h)==0);close(gfx);
    printf("SESSION_PASSED %s %u checks old_process=%lu old_instance=%lu\n",nested?"nested":"native",checks,(unsigned long)old_process,(unsigned long)old_instance);
    return 0;
}
