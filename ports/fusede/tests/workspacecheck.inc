/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <sys/wait.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/select.h>
#include "workspace-wire.h"
static void workspace_fail(unsigned line) {fprintf(stderr,"FDE_WORKSPACE_FAILED line %u\n",line);exit(1);}
#define WSCHECK(c) do{if(!(c))workspace_fail(__LINE__);}while(0)
static int workspace_rejected(unsigned version,unsigned op,unsigned extra) {
    struct sockaddr_un address;nxOpenReq open;struct nxr_workspace_req q;
    char packet[sizeof(q)+1],byte;unsigned long ack;fd_set reads;struct timeval timeout;
    int fd=socket(AF_UNIX,SOCK_STREAM,0),result;
    if(fd<0)return 0;
    memset(&address,0,sizeof(address));address.sun_family=AF_UNIX;strcpy(address.sun_path,GR_NAMED_SOCKET);
    if(connect(fd,(struct sockaddr *)&address,sizeof(address))<0){close(fd);return 0;}
    memset(&open,0,sizeof(open));open.reqType=GrNumOpen;open.length=sizeof(open);open.c16_magic=0x4e583102UL;
    if(write(fd,&open,sizeof(open))!=sizeof(open)||read(fd,&ack,sizeof(ack))!=sizeof(ack)||ack!=open.c16_magic){close(fd);return 0;}
    memset(&q,0,sizeof(q));q.header.reqType=NXR_WORKSPACE;q.header.length=sizeof(q)+extra;q.version=version;q.op=op;
    memcpy(packet,&q,sizeof(q));packet[sizeof(q)]=0;
    if(write(fd,packet,sizeof(q)+extra)!=sizeof(q)+extra){close(fd);return 0;}
    FD_ZERO(&reads);FD_SET(fd,&reads);timeout.tv_sec=2;timeout.tv_usec=0;
    result=select(fd+1,&reads,NULL,NULL,&timeout)>0&&read(fd,&byte,1)==0;
    close(fd);return result;
}
static int workspace_foreign(GR_WINDOW_ID id) {
    WSCHECK(fde_window_role(id,FDE_ROLE_PANEL,0,FDE_EDGE_TOP,24)==FDE_DENIED);
    WSCHECK(fde_workspace_frame(id,id)==FDE_DENIED);return 0;
}
static void wait_frame(GR_WINDOW_ID id,int decorated) {
    unsigned i;struct fde_window_info w;
    for(i=0;i<100;i++){WSCHECK(fde_window_get(id,&w)==FDE_OK);
        if(!decorated||(w.state&FDE_WINDOW_DECORATED))return;usleep(20000);}
    workspace_fail(__LINE__);
}
static int workspace_check(const char *self) {
    struct fde_workspace_info before,area;struct fde_window_info w,original;
    GR_WINDOW_ID a,b,p;GR_EVENT e;GR_WM_PROPERTIES props;GR_WINDOW_INFO wi;
    unsigned i,notices=0;int pid,status,hosted;
    WSCHECK(fde_workspace_get(&before)==FDE_OK);hosted=(before.capabilities&FDE_CAP_HOSTED)!=0;
    for(i=0;!hosted&&!(before.capabilities&FDE_CAP_MAXIMIZE)&&i<100;i++){
        usleep(20000);WSCHECK(fde_workspace_get(&before)==FDE_OK);}
    WSCHECK(hosted||(before.capabilities&FDE_CAP_MAXIMIZE));
    WSCHECK(fde_workspace_manage()==(hosted?FDE_UNSUPPORTED:FDE_BUSY));
    WSCHECK(workspace_rejected(0,NXR_WS_INFO,0));
    WSCHECK(workspace_rejected(NXR_WORKSPACE_VERSION,99,0));
    WSCHECK(workspace_rejected(NXR_WORKSPACE_VERSION,NXR_WS_INFO,1));
    a=GrNewWindowEx(GR_WM_PROPS_APPWINDOW,"Workspace A",GR_ROOT_WINDOW_ID,50,70,200,120,0);
    b=GrNewWindowEx(GR_WM_PROPS_APPWINDOW,"Workspace B",GR_ROOT_WINDOW_ID,80,100,180,100,0);
    p=GrNewWindow(GR_ROOT_WINDOW_ID,0,0,240,24,0,GR_RGB(30,40,50),0);
    WSCHECK(a&&b&&p);
    memset(&props,0,sizeof(props));props.flags=GR_WM_FLAGS_PROPS;props.props=GR_WM_PROPS_NODECORATE;
    GrSetWMProperties(p,&props);
    WSCHECK(fde_window_get(p,&w)==FDE_OK); /* decoration is independent of role */
    WSCHECK(fde_window_role(p,FDE_ROLE_PANEL,0,FDE_EDGE_TOP,65535U)==FDE_INVALID);
    WSCHECK(fde_window_role(p,FDE_ROLE_PANEL,0,FDE_EDGE_TOP,24)==FDE_OK);
    WSCHECK(fde_window_role(a,FDE_ROLE_DIALOG,a,0,0)==FDE_INVALID);
    WSCHECK(fde_window_role(b,FDE_ROLE_DIALOG,a,0,0)==FDE_OK);
    WSCHECK(fde_window_role(a,FDE_ROLE_DIALOG,b,0,0)==FDE_INVALID);
    GrSelectEvents(a,GR_EVENT_MASK_CLOSE_REQ);GrMapWindow(a);GrMapWindow(b);GrFlush();
    wait_frame(a,!hosted);wait_frame(b,!hosted);
    WSCHECK(fde_window_get(a,&original)==FDE_OK);
    WSCHECK(fde_window_role(a,FDE_ROLE_NORMAL,0,0,0)==FDE_BUSY);
    WSCHECK(fde_window_action(a,FDE_ACTIVATE)==FDE_OK);
    WSCHECK(fde_window_get(a,&w)==FDE_OK&&(w.state&FDE_WINDOW_ACTIVE));
    GrMapWindow(p);if(!hosted)GrSetFocus(p);GrFlush();
    WSCHECK(fde_workspace_get(&area)==FDE_OK);
    WSCHECK(area.y==(hosted?before.y:24)&&area.height==(hosted?before.height:before.height-24));
    WSCHECK(fde_window_get(a,&w)==FDE_OK&&(w.state&FDE_WINDOW_ACTIVE));
    WSCHECK(fde_window_action(p,FDE_ACTIVATE)==FDE_OK);
    WSCHECK(fde_window_get(p,&w)==FDE_OK&&(w.state&FDE_WINDOW_ACTIVE));
    WSCHECK(fde_window_action(a,FDE_ACTIVATE)==FDE_OK);
    GrGetWindowInfo(p,&wi);WSCHECK(wi.parent==GR_ROOT_WINDOW_ID);
    WSCHECK(fde_window_action(a,FDE_MAXIMIZE)==(hosted?FDE_UNSUPPORTED:FDE_OK));
    if(!hosted){WSCHECK(fde_window_get(a,&w)==FDE_OK&&(w.state&FDE_WINDOW_MAXIMIZED)&&w.y==24);
        GrGetWindowInfo(a,&wi);GrGetWindowInfo(wi.parent,&wi);
        WSCHECK(wi.x==area.x&&wi.y==area.y&&wi.width==area.width&&wi.height==area.height);}
    WSCHECK(fde_window_action(a,FDE_MINIMIZE)==FDE_OK);
    WSCHECK(fde_window_get(a,&w)==FDE_OK&&(w.state&FDE_WINDOW_MINIMIZED)&&!(w.state&FDE_WINDOW_MAPPED));
    WSCHECK(fde_window_action(a,FDE_RESTORE)==FDE_OK);
    WSCHECK(fde_window_get(a,&w)==FDE_OK&&!(w.state&(FDE_WINDOW_MINIMIZED|FDE_WINDOW_MAXIMIZED)));
    WSCHECK(w.x==original.x&&w.y==original.y&&w.width==original.width&&w.height==original.height);
    WSCHECK(fde_workspace_watch(1)==FDE_OK);
    memset(&props,0,sizeof(props));props.flags=GR_WM_FLAGS_TITLE;props.title="Workspace renamed";
    for(i=0;i<12;i++)GrSetWMProperties(a,&props);GrFlush();
    WSCHECK(fde_workspace_get(&area)==FDE_OK&&area.revision!=before.revision);
    for(i=0;i<64;i++){GrCheckNextEvent(&e);if(e.type==GR_EVENT_TYPE_NONE)break;if(e.type==FDE_EVENT_WORKSPACE)++notices;}
    WSCHECK(notices==1);WSCHECK(fde_workspace_watch(0)==FDE_OK);
    WSCHECK(fde_window_get(a,&w)==FDE_OK&&!strcmp(w.title,"Workspace renamed"));
    pid=fork();WSCHECK(pid>=0);
    if(!pid){char id[16];char *args[4];sprintf(id,"%u",a);args[0]=(char *)self;args[1]="--foreign";args[2]=id;args[3]=NULL;execvp(self,args);_exit(1);}
    WSCHECK(waitpid(pid,&status,0)==pid&&!status);
    WSCHECK(fde_window_action(a,FDE_CLOSE)==FDE_OK);
    for(i=0;i<64;i++){GrCheckNextEvent(&e);if(e.type==GR_EVENT_TYPE_CLOSE_REQ)break;}
    WSCHECK(i<64);WSCHECK(fde_window_get(a,&w)==FDE_OK);
    GrDestroyWindow(p);GrFlush();WSCHECK(fde_workspace_get(&area)==FDE_OK&&area.y==before.y&&area.height==before.height);
    GrDestroyWindow(b);GrDestroyWindow(a);GrFlush();WSCHECK(fde_window_get(a,&w)==FDE_INVALID);
    WSCHECK(fde_window_action(a,FDE_ACTIVATE)==FDE_INVALID);
    puts("FDE_WORKSPACE_CHECKS_OK");fflush(stdout);return 0;
}
