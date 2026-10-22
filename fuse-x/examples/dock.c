/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Disposable guest fixture for grouped desktop dock acceptance tests. */
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

#define C(x) do { if (!(x)) { fprintf(stderr,"DOCK_FAILED %u errno %d: %s\n",__LINE__,errno,#x); exit(1); } } while (0)
static int gfx;
static const char *tag;

static void session(struct gx_session *s,unsigned operation,struct gx_session_record *r) {
    memset(s,0,sizeof(*s));s->version=GXS_VERSION;s->operation=operation;s->record=r;
}
static void register_app(const char *id,const char *name) {
    struct gx_session s;struct gx_session_record r;memset(&r,0,sizeof(r));
    strcpy(r.app_id,id);strcpy(r.name,name);strcpy(r.icon,"application");
    session(&s,GXS_REGISTER,&r);C(ioctl(gfx,GX_SESSION,&s)==0);
}
static struct gx_window create(const char *title,unsigned hidden) {
    struct gx_window w;struct gx_fill fill;struct gx_handle h;
    memset(&w,0,sizeof(w));w.version=GX_VERSION;w.width=240;w.height=160;w.format=GX_RGB565;
    strcpy(w.title,title);C(ioctl(gfx,hidden?GX_CREATE_HIDDEN:GX_CREATE,&w)==0);
    memset(&fill,0,sizeof(fill));fill.version=GX_VERSION;fill.surface=w.surface;
    fill.width=w.width;fill.height=w.height;fill.colour=strcmp(tag,"beta")?0x2947:0x528b;
    C(ioctl(gfx,GX_FILL,&fill)==0);h.version=GX_VERSION;h.handle=w.window;C(ioctl(gfx,GX_PRESENT,&h)==0);
    return w;
}
static void destroy(struct gx_window *w) {
    struct gx_handle h;if(!w->window)return;
    h.version=GX_VERSION;h.handle=w->window;C(ioctl(gfx,GX_DESTROY,&h)==0);w->window=0;
}
static void events(struct gx_window *w) {
    struct gx_event e;if(!w->window)return;
    do {
        memset(&e,0,sizeof(e));e.version=GX_VERSION;e.window=w->window;C(ioctl(gfx,GX_EVENT,&e)==0);
        if(e.type==GX_BUTTON&&(e.flags&GX_DOWN)) {
            printf("DOCK_CLICK %s %u %ld %ld\n",tag,e.code,(long)e.x,(long)e.y);fflush(stdout);
        }
        if(e.type==GX_KEY&&(e.flags&GX_DOWN)) {
            printf("DOCK_KEY %s %u\n",tag,e.code);fflush(stdout);
        }
        if(e.type==GX_RESIZE) {
            struct gx_configure c;memset(&c,0,sizeof(c));c.version=GX_VERSION;c.window=w->window;
            c.width=e.x;c.height=e.y;C(ioctl(gfx,GX_CONFIGURE,&c)==0);
        }
        if(e.type==GX_CLOSE) {
            destroy(w);printf("DOCK_CLOSED %s\n",tag);fflush(stdout);return;
        }
    }while(e.type);
}
static int attach_helper(void) {
    int pipefd[2],pid;struct gx_session s;C(pipe(pipefd)==0);pid=fork();C(pid>=0);
    if(!pid) {
        char c;close(pipefd[1]);close(gfx);
        while(read(pipefd[0],&c,1)>0) {}
        close(pipefd[0]);_exit(0);
    }
    close(pipefd[0]);session(&s,GXS_ATTACH_CHILD,NULL);s.object=pid;C(ioctl(gfx,GX_SESSION,&s)==0);
    printf("DOCK_HELPER %s %u\n",tag,pid);fflush(stdout);return pipefd[1];
}
static int controller(void) {
    static const char *names[]={"alpha","beta","other","legacy"};
    int commands[4]={-1,-1,-1,-1},children[4]={0,0,0,0};char line[64];unsigned i,j;
    printf("DOCK_CONTROL_READY\n");fflush(stdout);
    while(fgets(line,sizeof(line),stdin)) {
        if(!strncmp(line,"quit",4))break;
        if(!strncmp(line,"start ",6)) {
            for(i=0;i<4;i++)if(!strncmp(line+6,names[i],strlen(names[i])))break;
            C(i<4&&commands[i]<0);
            { int channel[2],pid;C(pipe(channel)==0);pid=fork();C(pid>=0);
                if(!pid) {
                    char input[12];char *args[]={"dock-probe",(char *)names[i],input,NULL};
                    close(channel[1]);for(j=0;j<4;j++)if(commands[j]>=0)close(commands[j]);
                    snprintf(input,sizeof(input),"%u",channel[0]);execv("/bin/dock-probe",args);_exit(92);
                }
                close(channel[0]);commands[i]=channel[1];children[i]=pid;
            }
        } else {
            for(i=0;i<4;i++)if(!strncmp(line,names[i],strlen(names[i]))&&line[strlen(names[i])]==':')break;
            C(i<4&&commands[i]>=0);
            C(write(commands[i],line+strlen(names[i])+1,1)==1);
            if(line[strlen(names[i])+1]=='q') { close(commands[i]);commands[i]=-1; }
        }
    }
    for(i=0;i<4;i++)if(commands[i]>=0)close(commands[i]);
    for(i=0;i<4;i++)if(children[i])C(waitpid(children[i],NULL,0)==children[i]);
    printf("DOCK_CONTROL_DONE\n");fflush(stdout);return 0;
}
int main(int argc,char **argv) {
    struct gx_window primary,hidden;struct gx_title title;struct gx_handle h;
    int helper=-1,running=1,input;char command;const char *caption;
    if(argc==2&&!strcmp(argv[1],"--controller"))return controller();
    C(argc==3);tag=argv[1];input=atoi(argv[2]);gfx=open("/dev/gfx",O_RDWR);C(gfx>=0);memset(&hidden,0,sizeof(hidden));
    if(!strcmp(tag,"alpha")||!strcmp(tag,"beta"))register_app("org.riplix.dock-probe","Dock probe");
    else if(!strcmp(tag,"other"))register_app("org.riplix.dock-other","Other app");
    else C(!strcmp(tag,"legacy"));
    caption=!strcmp(tag,"beta")?"Beta":!strcmp(tag,"legacy")?"Legacy":"Alpha";
    primary=create(caption,0);
    if(!strcmp(tag,"alpha")) { helper=attach_helper();hidden=create("Hidden surface",1); }
    printf("DOCK_READY %s %u %lu\n",tag,getpid(),(unsigned long)primary.window);fflush(stdout);
    while(running) {
        fd_set ready;int nfds=gfx>input?gfx:input;
        FD_ZERO(&ready);FD_SET(gfx,&ready);FD_SET(input,&ready);C(select(nfds+1,&ready,NULL,NULL,NULL)>=0);
        if(FD_ISSET(input,&ready)) {
            int count=read(input,&command,1);
            if(count==0)running=0;
            else if(count==1) {
                if(command=='t'&&primary.window) {
                    memset(&title,0,sizeof(title));title.version=GX_VERSION;title.window=primary.window;
                    strcpy(title.title,"Alpha renamed Ω");C(ioctl(gfx,GX_TITLE,&title)==0);
                } else if(command=='s'&&hidden.window) {
                    struct gx_configure c;memset(&c,0,sizeof(c));c.version=GX_VERSION;
                    c.window=hidden.window;c.width=hidden.width;c.height=hidden.height;
                    C(ioctl(gfx,GX_CONFIGURE,&c)==0);
                    h.version=GX_VERSION;h.handle=hidden.window;C(ioctl(gfx,GX_ACTIVATE,&h)==0);
                } else if(command=='d')destroy(&hidden);
                else if(command=='x')destroy(&primary);
                else if(command=='q')running=0;
                printf("DOCK_COMMAND %s %c\n",tag,command);fflush(stdout);
            }
        }
        events(&primary);events(&hidden);
    }
    destroy(&primary);destroy(&hidden);close(gfx);close(input);if(helper>=0) { close(helper);wait(NULL); }
    return 0;
}
