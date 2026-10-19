/* SPDX-License-Identifier: GPL-2.0-only */
/* Guest ownership and Unix waits stay in FUZIX; the host owns surface storage. */
#include "host.h"
#include <kdata.h>
#include <native_hosted.h>
#include <gx.h>
#include "graphics.h"
#include "pty.h"
#include <vm_devices.h>
#define WINDOWS 16
static struct lease { ptptr owner; unsigned pid; unsigned long handle; } leases[WINDOWS];
static unsigned waiting;
unsigned fuse_new_terminals;
static int fail(unsigned e) { udata.u_error=e;return -1; }
#include "raster-abi.h"
static long host(unsigned op,unsigned long *a) {
    long result=rcc_hosted_gx(op,a);
    if(result<0)udata.u_error=-result;
    return result;
}
static struct lease *mine(unsigned long handle) {
    unsigned i;
    for(i=0;i<WINDOWS;i++)if(leases[i].handle==handle&&handle) {
        if(leases[i].owner==udata.u_ptab&&leases[i].pid==udata.u_ptab->p_pid)return &leases[i];
        fail(EPERM);return NULL;
    }
    fail(EINVAL);return NULL;
}
void ripple_gfx_wakeup(void) { if(waiting)wakeup(&waiting);selwake_dev(4,0,SELECT_IN); }
void ripple_gfx_poll(void) {
    unsigned i;unsigned long a[12]={0};
    if(rcc_hosted_gx(11,a)>0 && fuse_new_terminals<16) {
        ++fuse_new_terminals;if(init_process)pwake(init_process);
    }
    for(i=0;i<WINDOWS;i++)if(leases[i].handle) {
        a[0]=leases[i].handle;
        if(rcc_hosted_gx(10,a)>0){ripple_gfx_wakeup();break;}
    }
}
unsigned ripple_gfx_status(void) {
    unsigned i;unsigned long a[12]={0};
    for(i=0;i<WINDOWS;i++)if(leases[i].owner==udata.u_ptab&&leases[i].pid==udata.u_ptab->p_pid&&leases[i].handle) {
        a[0]=leases[i].handle;
        if(rcc_hosted_gx(10,a)>0)return SELECT_IN|SELECT_OUT;
    }
    return SELECT_OUT;
}
void ripple_gfx_exit(ptptr p) {
    unsigned i;unsigned long a[12]={0};
    for(i=0;i<WINDOWS;i++)if(leases[i].owner==p&&leases[i].handle) {
        a[0]=leases[i].handle;rcc_hosted_gx(2,a);memset(&leases[i],0,sizeof(leases[i]));
    }
}
int ripple_gfx_open(uint_fast8_t m,uint16_t f) {used(f);return m?fail(ENXIO):0;}
/* An inherited descriptor is not ownership of the parent's windows. */
int ripple_gfx_close(uint_fast8_t m) {used(m);return 0;}
int ripple_gfx_ioctl(uint_fast8_t m,uarg_t req,char *user) {
    struct gx_handle handle;struct lease *lease;
    unsigned long a[12]={0};
    if(m)return fail(ENXIO);
    if(req==GX_RASTER_FILL||req==GX_RASTER_COPY){
        struct gx_raster r;if(uget(user,&r,sizeof(r))||raster_request(&r,req==GX_RASTER_COPY))return -1;
        a[0]=(unsigned long)r.dst;a[1]=(unsigned long)r.src;a[2]=r.width;a[3]=r.height;
        a[4]=r.dst_stride;a[5]=r.src_stride;a[6]=r.colour;
        return host(req==GX_RASTER_FILL?15:16,a)<0?-1:0;
    }
    if(req==GX_INFO) {
        struct gx_info info={GX_VERSION,GX_CAP_COPY|GX_CAP_FILL|GX_CAP_INPUT|GX_CAP_MULTIWINDOW|GX_CAP_TITLE|GX_CAP_RESIZE|GX_CAP_DESKTOP_POINTER|GX_CAP_RASTER|GX_CAP_SCALED_UPLOAD|GX_CAP_INDEXED_UPLOAD|GX_CAP_CLIPBOARD,GX_HOSTED_MAX_WIDTH,GX_HOSTED_MAX_HEIGHT,WINDOWS,GX_RGB565,(unsigned long)GX_HOSTED_MAX_WIDTH*GX_HOSTED_MAX_HEIGHT};
        return uput(&info,user,sizeof(info));
    }
    if(req==GX_CREATE||req==GX_CREATE_HIDDEN) {
        struct gx_window w;unsigned i;
        if(uget(user,&w,sizeof(w))||!valaddr_w(user,sizeof(w),sizeof(w)))return -1;
        if(w.version!=GX_VERSION||w.format!=GX_RGB565||!w.width||!w.height||w.width>GX_HOSTED_MAX_WIDTH||w.height>GX_HOSTED_MAX_HEIGHT)return fail(EINVAL);
        for(i=0;i<WINDOWS&&leases[i].handle;i++);if(i==WINDOWS)return fail(ENOMEM);
        w.title[63]=0;a[0]=w.width;a[1]=w.height;a[2]=(unsigned long)w.title;a[3]=req==GX_CREATE_HIDDEN?GX_WINDOW_HIDDEN:0;
        if(host(1,a)<0)return -1;
        leases[i].owner=udata.u_ptab;leases[i].pid=udata.u_ptab->p_pid;leases[i].handle=a[0];
        w.window=w.surface=a[0];return uput(&w,user,sizeof(w));
    }
    if(uget(user,&handle,sizeof(handle)))return -1;
    if(handle.version!=GX_VERSION)return fail(EINVAL);
    lease=mine(handle.handle);if(!lease)return -1;
    a[0]=handle.handle;
    if(req==GX_CLIPBOARD_READ||req==GX_CLIPBOARD_WRITE) {
        struct gx_clipboard c;
        if(uget(user,&c,sizeof(c))||!valaddr_w(user,sizeof(c),sizeof(c)))return -1;
        if(c.length>GX_CLIPBOARD_MAX||c.capacity>GX_CLIPBOARD_MAX)return fail(EINVAL);
        if(req==GX_CLIPBOARD_READ) {
            if(!c.capacity||!valaddr_w(c.text,c.capacity,c.capacity))return fail(EFAULT);
        } else if(c.length&&!valaddr_r(c.text,c.length,c.length))return -1;
        a[1]=(unsigned long)c.text;a[2]=req==GX_CLIPBOARD_READ?c.capacity:c.length;
        if(host(req==GX_CLIPBOARD_READ?19:20,a)<0)return -1;
        c.length=a[2];return uput(&c,user,sizeof(c));
    }
    if(req==GX_DESKTOP_POINTER) {
        struct gx_pointer p;
        if(!valaddr_w(user,sizeof(p),sizeof(p))||host(14,a)<0)return -1;
        p.version=GX_VERSION;p.window=handle.handle;p.x=a[1];p.y=a[2];
        return uput(&p,user,sizeof(p));
    }
    if(req==GX_CONFIGURE) {
        struct gx_configure c;if(uget(user,&c,sizeof(c)))return -1;
        if(c.flags&~(GX_WINDOW_FIXED|GX_WINDOW_HIDDEN))return fail(EINVAL);
        a[1]=c.width;a[2]=c.height;a[3]=c.flags;return host(12,a)<0?-1:0;
    }
    if(req==GX_DESTROY) { if(host(2,a)<0)return -1;memset(lease,0,sizeof(*lease));return 0; }
    if(req==GX_PRESENT)return host(4,a)<0?-1:0;
    if(req==GX_ACTIVATE)return host(13,a)<0?-1:0;
    if(req==GX_UPLOAD||req==GX_UPLOAD_SCALED||req==GX_UPLOAD_INDEXED) {
        struct gx_upload u;unsigned long count;
        if(req==GX_UPLOAD_INDEXED) {
            struct gx_indexed_upload indexed;
            if(uget(user,&indexed,sizeof(indexed)))return -1;
            u=indexed.image;
            if(!valaddr_r(indexed.palette,256,256))return -1;
            a[7]=(unsigned long)indexed.palette;
        } else if(uget(user,&u,sizeof(u)))return -1;
        if(u.stride<u.width)return fail(EINVAL);
        if(req!=GX_UPLOAD&&(u.x||u.y||!u.width||!u.height||u.width>GX_HOSTED_MAX_WIDTH||u.height>GX_HOSTED_MAX_HEIGHT))return fail(EINVAL);
        if(!u.width||!u.height)return 0;
        count=(unsigned long)(u.height-1)*u.stride+u.width;
        if(!valaddr_r(u.pixels,count,count))return -1;
        a[1]=u.x;a[2]=u.y;a[3]=u.width;a[4]=u.height;a[5]=u.stride;a[6]=(unsigned long)u.pixels;
        return host(req==GX_UPLOAD_INDEXED?18:req==GX_UPLOAD_SCALED?17:3,a)<0?-1:0;
    }
    if(req==GX_FILL) {
        struct gx_fill f;if(uget(user,&f,sizeof(f)))return -1;
        a[1]=f.x;a[2]=f.y;a[3]=f.width;a[4]=f.height;a[5]=f.colour;
        return host(5,a)<0?-1:0;
    }
    if(req==GX_COPY) {
        struct gx_copy c;if(uget(user,&c,sizeof(c)))return -1;
        a[1]=c.sx;a[2]=c.sy;a[3]=c.width;a[4]=c.height;a[5]=c.dx;a[6]=c.dy;
        return host(6,a)<0?-1:0;
    }
    if(req==GX_TITLE) {
        struct gx_title w;if(uget(user,&w,sizeof(w)))return -1;
        w.title[63]=0;a[1]=(unsigned long)w.title;return host(7,a)<0?-1:0;
    }
    if(req==GX_EVENT) {
        struct gx_event e;if(!valaddr_w(user,sizeof(e),sizeof(e)))return -1;
        a[1]=(unsigned long)&e;if(host(8,a)<0)return -1;return uput(&e,user,sizeof(e));
    }
    if(req==GX_STATE) {
        struct gx_state s;if(!valaddr_w(user,sizeof(s),sizeof(s)))return -1;
        a[1]=(unsigned long)&s;if(host(9,a)<0)return -1;return uput(&s,user,sizeof(s));
    }
    if(req==GX_WAIT) {
        struct gx_wait w;VmHostTime now;inoptr ino;unsigned dev;irqflags_t irq;
        if(uget(user,&w,sizeof(w))||!valaddr_w(user,sizeof(w),sizeof(w)))return -1;
        if(w.flags&~(GX_READY_INPUT|GX_READY_READ|GX_READY_WRITE)||w.timeout>600)return fail(EINVAL);
        ino=getinode(w.master);if(!ino)return -1;dev=ino->c_node.i_addr[0];
        if(getmode(ino)!=MODE_R(F_CDEV)||major(dev)!=3||minor(dev)>=RIPPLE_PTY_COUNT)return fail(EINVAL);
        irq=di();waiting++;
        if(w.timeout){udata.u_ptab->p_timeout=w.timeout+1;ptimer_insert();}
        for(;;) {
            w.ready=ripple_pty_status(minor(dev));
            if(rcc_hosted_gx(10,a)>0)w.ready|=GX_READY_INPUT;
            w.ready&=w.flags;
            if(w.ready||!w.timeout||udata.u_ptab->p_timeout==1)break;
            if(psleep_flags(&waiting,0))break;
        }
        --waiting;udata.u_ptab->p_timeout=0;irqrestore(irq);
        if(udata.u_error)return -1;
        if(vm_host_monotonic(&now)!=VM_DEVICE_OK)return fail(EIO);
        w.milliseconds=now.seconds_low*1000UL+now.nanoseconds/1000000UL;
        return uput(&w,user,sizeof(w));
    }
    return fail(EOPNOTSUPP);
}
