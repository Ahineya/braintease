/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "fde_workspace.h"
static void usage(void) {
    puts("usage: fdectl [list | info ID | activate ID | minimize ID | maximize ID | restore ID | close ID]");
}
static int hold(int panel,unsigned edge,unsigned extent) {
    GR_WINDOW_ID w;GR_EVENT e;GR_GC_ID gc;
    w=GrNewWindowEx(GR_WM_PROPS_APPWINDOW,panel?"Workspace panel":"Workspace probe",GR_ROOT_WINDOW_ID,40,60,240,120,GR_RGB(32,40,46));
    if(!w)return 1;
    if(panel&&fde_window_role(w,FDE_ROLE_PANEL,0,edge,extent)!=FDE_OK)return 1;
    GrSelectEvents(w,GR_EVENT_MASK_EXPOSURE|GR_EVENT_MASK_CLOSE_REQ|GR_EVENT_MASK_KEY_DOWN);
    gc=GrNewGC();GrSetGCForeground(gc,GR_RGB(235,238,236));GrSetGCUseBackground(gc,GR_FALSE);
    GrMapWindow(w);GrFlush();printf("FDE_WORKSPACE_HOLD %u\n",w);fflush(stdout);
    for(;;){GrGetNextEvent(&e);if(e.type==GR_EVENT_TYPE_CLOSE_REQ)break;
        if(e.type==GR_EVENT_TYPE_EXPOSURE){GrText(w,gc,10,8,panel?"Workspace panel":"Window-management probe",-1,GR_TFTOP);GrFlush();}
        if(e.type==GR_EVENT_TYPE_KEY_DOWN){printf("FDE_WORKSPACE_KEY %u\n",e.keystroke.ch);fflush(stdout);}}
    GrDestroyGC(gc);GrDestroyWindow(w);return 0;
}
#include "tests/workspacecheck.inc"
int main(int argc,char **argv) {
    struct fde_workspace_info area;struct fde_window_info w;
    GR_WINDOW_ID id=0;unsigned action=0;int status=FDE_OK;
    if(argc==2&&!strcmp(argv[1],"--help")){usage();return 0;}
    if(GrOpen()<0)return 1;
    if(argc==2&&!strcmp(argv[1],"--check")){status=workspace_check(argv[0]);GrClose();return status;}
    if(argc==3&&!strcmp(argv[1],"--foreign")){status=workspace_foreign(strtoul(argv[2],NULL,10));GrClose();return status;}
    if(argc>=2&&!strcmp(argv[1],"--hold")){status=hold(0,0,0);GrClose();return status;}
    if(argc==4&&!strcmp(argv[1],"--panel")){status=hold(1,atoi(argv[2]),atoi(argv[3]));GrClose();return status;}
    if(argc==1||(argc==2&&!strcmp(argv[1],"list"))){
        status=fde_workspace_get(&area);
        if(!status){if(area.capabilities&FDE_CAP_HOSTED)printf("Native workspace; %u windows\n",area.count);
            else printf("Workspace %u,%u %ux%u; %u windows\n",area.x,area.y,area.width,area.height,area.count);
            puts("ID     STATE       TITLE");
            while(!(status=fde_window_next(id,&w))&&w.id){id=w.id;
                printf("%-6u %c%c%c%c        %s\n",w.id,w.state&FDE_WINDOW_ACTIVE?'*':' ',
                    w.state&FDE_WINDOW_MINIMIZED?'m':' ',w.state&FDE_WINDOW_MAXIMIZED?'M':' ',
                    w.role==FDE_ROLE_PANEL?'p':' ',w.title);}}
    }else if(argc==3){
        char *end;unsigned long parsed=strtoul(argv[2],&end,10);
        if(*end||!parsed||parsed>65535UL){usage();GrClose();return 2;}id=parsed;
        if(!strcmp(argv[1],"info")){
            status=fde_window_get(id,&w);
            if(!status)printf("Window %u: %ux%u at %d,%d state=%u %s\n",w.id,w.width,w.height,w.x,w.y,w.state,w.title);
            GrClose();return status?1:0;
        }
        if(!strcmp(argv[1],"activate"))action=FDE_ACTIVATE;
        if(!strcmp(argv[1],"minimize"))action=FDE_MINIMIZE;
        if(!strcmp(argv[1],"maximize"))action=FDE_MAXIMIZE;
        if(!strcmp(argv[1],"restore"))action=FDE_RESTORE;
        if(!strcmp(argv[1],"close"))action=FDE_CLOSE;
        if(action)status=fde_window_action(id,action);else status=FDE_INVALID;
    }else{usage();GrClose();return 2;}
    if(status)fprintf(stderr,"fdectl: %s\n",status==FDE_UNSUPPORTED?"operation is managed by the host or unavailable":
        status==FDE_DENIED?"operation denied":status==FDE_INVALID?"invalid command or stale window ID":"operation failed");
    GrClose();return status?1:0;
}
