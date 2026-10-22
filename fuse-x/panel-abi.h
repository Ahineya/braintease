/* SPDX-License-Identifier: GPL-2.0-only */
/* Manager authority is process-bound in the kernel, never a host PID guess. */
#include <gx-panel.h>
#define GXP_HOST_REVOKE 64
static int panel_ioctl(char *user) {
    struct gx_panel request;
    struct session_process *entry;
    unsigned long args[12]={0},cells;
    if(uget(user,&request,sizeof(request))||!valaddr_w(user,sizeof(request),sizeof(request)))return -1;
    if(request.version!=GXP_VERSION||request.operation>GXP_RELEASE)return fail(EINVAL);
    if(request.operation==GXP_INFO) {
        if(request.authority||request.window||request.flags||request.count||request.top||request.bottom||request.regions)return fail(EINVAL);
    } else {
        entry=session_find(udata.u_ptab,0);
        if(!entry||!manager_token||request.authority!=manager_token||manager_owner!=udata.u_ptab||manager_pid!=entry->pid)return fail(EPERM);
        args[1]=entry->id;
        if(request.operation==GXP_CONFIGURE) {
            if(!mine(request.window))return -1;
            if((request.flags&~(GXP_KEYBOARD|GXP_MODAL|GXP_ALPHA))||((request.flags&GXP_MODAL)&&!(request.flags&GXP_KEYBOARD)))return fail(EINVAL);
            if(request.count) {
                if(!request.regions)return fail(EINVAL);
                cells=(unsigned long)request.count*sizeof(struct gx_panel_rect);
                if(!valaddr_r((const uint8_t *)request.regions,cells,cells))return -1;
            } else if(request.regions||(request.flags&~GXP_ALPHA)||request.top||request.bottom)return fail(EINVAL);
        } else if(request.flags||request.count||request.top||request.bottom||request.regions)return fail(EINVAL);
    }
    args[0]=request.operation;args[2]=request.window;args[3]=request.flags;
    args[4]=request.count;args[5]=(unsigned long)request.regions;
    args[6]=request.top;args[7]=request.bottom;
    if(host(32,args)<0)return -1;
    request.flags=args[3];request.width=args[8];request.height=args[9];
    return uput(&request,user,sizeof(request));
}
static int panel_upload_ioctl(char *user) {
    struct gx_panel_upload request;
    struct session_process *entry;
    unsigned long args[12]={0},row,cells;
    if(uget(user,&request,sizeof(request)))return -1;
    if(request.version!=GXP_VERSION)return fail(EINVAL);
    entry=session_find(udata.u_ptab,0);
    if(!entry||!manager_token||request.authority!=manager_token||manager_owner!=udata.u_ptab||manager_pid!=entry->pid)return fail(EPERM);
    if(!mine(request.window))return -1;
    if(!request.width||!request.height||request.width>GX_HOSTED_MAX_WIDTH*2||request.height>GX_HOSTED_MAX_HEIGHT*2)return fail(EINVAL);
    row=(unsigned long)request.width*4;
    if(request.stride<row)return fail(EINVAL);
    if(request.height>1&&request.stride>(0xffffffffUL-row)/(request.height-1))return fail(EINVAL);
    cells=(unsigned long)(request.height-1)*request.stride+row;
    if(!valaddr_r((const uint8_t *)request.pixels,cells,cells))return -1;
    args[0]=3;args[1]=entry->id;args[2]=request.window;
    args[3]=request.x;args[4]=request.y;args[5]=request.width;args[6]=request.height;
    args[7]=request.stride;args[8]=(unsigned long)request.pixels;
    return host(32,args)<0?-1:0;
}
