/* SPDX-License-Identifier: GPL-2.0-only */
/* Guest ownership and Unix waits stay in Riplix; the host owns surface storage. */
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
#include "render-abi.h"
#include "session-abi.h"
#include "panel-abi.h"
void ripple_gfx_wakeup(void) { if(waiting)wakeup(&waiting);selwake_dev(4,0,SELECT_IN); }
void ripple_gfx_poll(void) {
    unsigned i;unsigned long a[12]={0};
    if(rcc_hosted_gx(11,a)>0 && fuse_new_terminals<16) {
        ++fuse_new_terminals;if(init_process)pwake(init_process);
    }
    if(session_any_changed())ripple_gfx_wakeup();
    for(i=0;i<WINDOWS;i++)if(leases[i].handle) {
        a[0]=leases[i].handle;
        if(rcc_hosted_gx(10,a)>0){ripple_gfx_wakeup();break;}
    }
}
unsigned ripple_gfx_status(void) {
    unsigned i;unsigned long a[12]={0};
    if(session_own_changed())return SELECT_IN|SELECT_OUT;
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
    session_exit(p);
}
int ripple_gfx_open(uint_fast8_t m,uint16_t f) {used(f);return m?fail(ENXIO):0;}
/* An inherited descriptor is not ownership of the parent's windows. */
int ripple_gfx_close(uint_fast8_t m) {used(m);return 0;}
int ripple_gfx_ioctl(uint_fast8_t m,uarg_t req,char *user) {
    struct gx_handle handle;struct lease *lease;
    unsigned long a[12]={0};
    if(m)return fail(ENXIO);
    if(req==GX_SESSION)return session_ioctl(user);
    if(req==GX_PANEL)return panel_ioctl(user);
    if(req==GX_PANEL_UPLOAD)return panel_upload_ioctl(user);
    if(req==GX_RENDER)return render_ioctl(user);
    if(req==GX_RASTER_A8){
        struct gx_raster_a8 r;unsigned long cells,list_cells,d,s;
        if(uget(user,&r,sizeof(r)))return -1;
        if(r.version!=GX_VERSION)return fail(EINVAL);
        if(!r.width&&!r.height&&!r.stride&&!r.flags&&!r.count&&!r.dst&&!r.primitives)
            return rcc_hosted_gx(29,a)<0?fail(ENOTTY):0;
        if(!r.width||!r.height||r.width>GX_HOSTED_MAX_WIDTH*2||
            r.height>GX_HOSTED_MAX_HEIGHT*2||r.stride<r.width||
            (r.flags&~GX_A8_CLEAR)||r.count>8192UL)return fail(EINVAL);
        cells=(unsigned long)(r.height-1)*r.stride+r.width;
        list_cells=r.count*sizeof(struct gx_a8_primitive);
        if(!valaddr_r(r.dst,cells,cells)||!valaddr_w(r.dst,cells,cells))return -1;
        if(r.count){
            if(!valaddr_r((const uint8_t *)r.primitives,list_cells,list_cells))return -1;
            d=(unsigned long)r.dst;s=(unsigned long)r.primitives;
            d=(d>>16)*64000UL+(unsigned)d;s=(s>>16)*64000UL+(unsigned)s;
            if(d<s+list_cells&&s<d+cells)return fail(EINVAL);
        }
        a[0]=(unsigned long)r.dst;a[1]=(unsigned long)r.primitives;
        a[2]=r.width;a[3]=r.height;a[4]=r.stride;a[5]=r.count;a[6]=r.flags;
        return host(29,a)<0?-1:0;
    }
    if(req==GX_RASTER_BGR888){
        struct gx_raster_bgr888 r;unsigned long count;
        if(uget(user,&r,sizeof(r)))return -1;
        if(r.version!=GX_VERSION||!r.width||!r.height||r.width>GX_HOSTED_MAX_WIDTH*2||
            r.height>GX_HOSTED_MAX_HEIGHT*2||r.dst_stride<(unsigned long)r.width*3||
            (r.mask&&r.mask_stride<r.width)||r.blue>255||r.green>255||r.red>255||r.opacity>255)return fail(EINVAL);
        count=(unsigned long)(r.height-1)*r.dst_stride+(unsigned long)r.width*3;
        if(!valaddr_r(r.dst,count,count)||!valaddr_w(r.dst,count,count))return -1;
        if(r.mask){count=(unsigned long)(r.height-1)*r.mask_stride+r.width;
            if(!valaddr_r(r.mask,count,count))return -1;}
        a[0]=(unsigned long)r.dst;a[1]=(unsigned long)r.mask;a[2]=r.width;a[3]=r.height;
        a[4]=r.dst_stride;a[5]=r.mask_stride;a[6]=r.blue;a[7]=r.green;a[8]=r.red;a[9]=r.opacity;
        return host(25,a)<0?-1:0;
    }
    if(req==GX_RASTER_FILL||req==GX_RASTER_COPY){
        struct gx_raster r;if(uget(user,&r,sizeof(r))||raster_request(&r,req==GX_RASTER_COPY))return -1;
        a[0]=(unsigned long)r.dst;a[1]=(unsigned long)r.src;a[2]=r.width;a[3]=r.height;
        a[4]=r.dst_stride;a[5]=r.src_stride;a[6]=r.colour;
        return host(req==GX_RASTER_FILL?15:16,a)<0?-1:0;
    }
    if(req==GX_INFO) {
        struct gx_info info={GX_VERSION,GX_CAP_COPY|GX_CAP_FILL|GX_CAP_INPUT|GX_CAP_MULTIWINDOW|GX_CAP_TITLE|GX_CAP_RESIZE|GX_CAP_DESKTOP_POINTER|GX_CAP_RASTER|GX_CAP_SCALED_UPLOAD|GX_CAP_INDEXED_UPLOAD|GX_CAP_CLIPBOARD,GX_HOSTED_MAX_WIDTH,GX_HOSTED_MAX_HEIGHT,WINDOWS,GX_RGB565,(unsigned long)GX_HOSTED_MAX_WIDTH*GX_HOSTED_MAX_HEIGHT};
        info.flags|=GX_CAP_BGR888_UPLOAD|GX_CAP_BACKING_SCALE|GX_CAP_BGR888_RASTER;
        info.flags|=GX_CAP_PRESENTATION_ZOOM;
        if(rcc_hosted_gx(21,a)>=0)info.flags|=GX_CAP_TERMINAL;
        /* Optional native output profile; older hosted backends retain the
         * established defaults. The native adapter advertises actual support. */
        memset(a,0,sizeof(a));
        if(rcc_hosted_gx(0,a)==0){
            info.flags=a[0];info.max_width=a[1];info.max_height=a[2];
            info.max_windows=a[3];info.max_surface_cells=(unsigned long)info.max_width*info.max_height;
        }
        return uput(&info,user,sizeof(info));
    }
    if(req==GX_FORMATS) {
        struct gx_formats f;
        if(uget(user,&f,sizeof(f)))return -1;
        if(f.version!=GX_VERSION)return fail(EINVAL);
        if(host(26,a)<0)return -1;
        f.preferred=a[1];f.formats=a[2];return uput(&f,user,sizeof(f));
    }
    if(req==GX_TERMINAL_METRICS){
        struct gx_terminal_metrics t;
        if(host(21,a)<0)return -1;
        t.version=GX_VERSION;t.width=a[1];t.height=a[2];return uput(&t,user,sizeof(t));
    }
    if(req==GX_CREATE||req==GX_CREATE_HIDDEN) {
        struct gx_window w;unsigned i;
        struct session_process *process;
        if(uget(user,&w,sizeof(w))||!valaddr_w(user,sizeof(w),sizeof(w)))return -1;
        if(w.version!=GX_VERSION||(w.format!=GX_RGB565&&w.format!=GX_XRGB8888)||!w.width||!w.height||w.width>GX_HOSTED_MAX_WIDTH||w.height>GX_HOSTED_MAX_HEIGHT)return fail(EINVAL);
        for(i=0;i<WINDOWS&&leases[i].handle;i++);if(i==WINDOWS)return fail(ENOMEM);
        process=session_find(udata.u_ptab,1);if(!process)return -1;
        w.title[63]=0;a[0]=w.width;a[1]=w.height;a[2]=(unsigned long)w.title;a[3]=req==GX_CREATE_HIDDEN?GX_WINDOW_HIDDEN:0;a[4]=w.format;
        a[5]=process->id;a[6]=process->pid;
        if(host(1,a)<0)return -1;
        leases[i].owner=udata.u_ptab;leases[i].pid=udata.u_ptab->p_pid;leases[i].handle=a[0];
        w.window=w.surface=a[0];return uput(&w,user,sizeof(w));
    }
    if(uget(user,&handle,sizeof(handle)))return -1;
    if(handle.version!=GX_VERSION)return fail(EINVAL);
    lease=mine(handle.handle);if(!lease)return -1;
    a[0]=handle.handle;
    if(req==GX_PRESENTATION_ZOOM){
        struct gx_presentation z;
        if(uget(user,&z,sizeof(z))||!valaddr_w(user,sizeof(z),sizeof(z)))return -1;
        if(z.percent<100||z.percent>300)return fail(EINVAL);
        a[1]=z.percent;
        if(host(28,a)<0)return -1;
        z.percent=a[1];return uput(&z,user,sizeof(z));
    }
    if(req==GX_BACKING_SCALE){
        struct gx_backing b;
        if(uget(user,&b,sizeof(b))||!valaddr_w(user,sizeof(b),sizeof(b)))return -1;
        if(b.scale>2)return fail(EINVAL);a[1]=b.scale;
        if(host(24,a)<0)return -1;
        b.scale=a[1];b.width=a[2];b.height=a[3];return uput(&b,user,sizeof(b));
    }
    if(req==GX_TERMINAL_DRAW){
        struct gx_terminal_draw t;
        if(uget(user,&t,sizeof(t)))return -1;
        if(!t.cols||t.cols>240||!t.rows||t.rows>100||t.row>=t.rows||t.length>(unsigned long)t.cols*67||
            t.cursor_visible>1||t.cursor_shape>6)return fail(EINVAL);
        if(!valaddr_r(t.data,t.length,t.length))return -1;
        a[1]=t.cols;a[2]=t.rows;a[3]=t.row;a[4]=(unsigned long)t.data;a[5]=t.length;
        a[6]=t.cursor_x;a[7]=t.cursor_y;a[8]=t.cursor_visible;a[9]=t.cursor_shape;
        return host(22,a)<0?-1:0;
    }
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
    if(req==GX_FILL_RGB888) {
        struct gx_fill_rgb888 f;if(uget(user,&f,sizeof(f)))return -1;
        if(f.colour>0xffffffUL)return fail(EINVAL);
        a[1]=f.x;a[2]=f.y;a[3]=f.width;a[4]=f.height;a[5]=f.colour;
        return host(27,a)<0?-1:0;
    }
    if(req==GX_UPLOAD_BGR888) {
        struct gx_upload u;unsigned long row,count;
        if(uget(user,&u,sizeof(u)))return -1;
        row=(unsigned long)u.width*3;
        if(u.stride<row)return fail(EINVAL);
        if(!u.width||!u.height)return 0;
        count=(unsigned long)(u.height-1)*u.stride+row;
        if(!valaddr_r(u.pixels,count,count))return -1;
        a[1]=u.x;a[2]=u.y;a[3]=u.width;a[4]=u.height;a[5]=u.stride;a[6]=(unsigned long)u.pixels;
        return host(23,a)<0?-1:0;
    }
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
