/* SPDX-License-Identifier: GPL-2.0-only */
#include "platform.h"
#include <kdata.h>
#include <mmio.h>
#include <vm_devices.h>
#include <vm_bulk.h>
#include <gx.h>
#include "graphics.h"
#include "pty.h"

#define EVENTS 32
typedef char scanout_fits_reservation[(640UL*480 <= MMU_GRAPHICS_CELLS) ? 1 : -1];
static ptptr owner;
static uint16_t owner_pid, width, height, head, queued, lost, closing;
static uint32_t generation;
static unsigned waiting;
void ripple_gfx_wakeup(void) { if(waiting)wakeup(&waiting); }
static VmInputEvent events[EVENTS], incoming[8];
static unsigned short fallback[640];
static unsigned long address(unsigned long at) {
    return ((unsigned long)(MMU_GRAPHICS_BANK + at / BANK_CELLS) << 16) | (unsigned)(at % BANK_CELLS);
}
static int fail(unsigned e) { udata.u_error=e; return -1; }
static int mine(uint32_t handle) {
    if (!owner || owner!=udata.u_ptab || owner_pid!=owner->p_pid) return fail(EPERM);
    if (!handle || handle!=generation) return fail(EINVAL);
    return 0;
}
static int rect(unsigned x,unsigned y,unsigned w,unsigned h) {
    return x<=width && y<=height && w<=width-x && h<=height-y;
}
/* Destination and source addresses are physical, after user-map validation. */
static int transfer(unsigned long to,unsigned sb,unsigned so,unsigned n,int fill,unsigned colour) {
    unsigned status, i; irqflags_t irq=di();

#if RIPPLE_BULK_IO
    if (fill) status=vm_bulk_fill((unsigned)(to>>16),(unsigned)to,n,colour);
    else status=vm_bulk_copy(sb,so,(unsigned)(to>>16),(unsigned)to,n,VM_BULK_WORDS);
#else
    status=VM_DEVICE_UNSUPPORTED;
#endif
    irqrestore(irq);
    if (status==VM_DEVICE_OK) return 0;
    if (status!=VM_DEVICE_UNKNOWN_COMMAND && status!=VM_DEVICE_UNSUPPORTED) return fail(EIO);
    if(!fill)for(i=0;i<n;i++) {
        unsigned off=so+i,bank=sb;
        if(off>=BANK_CELLS){bank++;off-=BANK_CELLS;}
        fallback[i]=dmem_load(bank,off);
    }
    {
        unsigned bank=(unsigned)(to>>16),off=(unsigned)to;
        for(i=0;i<n;i++) {
            dmem_store(bank,off,fill?colour:fallback[i]);
            if(++off==BANK_CELLS){++bank;off=0;}
        }
    }
    return 0;
}
static int fill_rect(unsigned x,unsigned y,unsigned w,unsigned h,unsigned colour) {
    unsigned row;if(!w||!h)return 0;
    if(!x&&w==width) {
        unsigned long at=(unsigned long)y*width,left=(unsigned long)w*h;
        while(left) {
            unsigned n=left>VM_BULK_MAX_CELLS?VM_BULK_MAX_CELLS:(unsigned)left;
            if(transfer(address(at),0,0,n,1,colour))return -1;
            at+=n;left-=n;
        }
        return 0;
    }
    for(row=0;row<h;row++) if(transfer(address((unsigned long)(y+row)*width+x),0,0,w,1,colour))return -1;
    return 0;
}
void ripple_gfx_exit(ptptr p) {
    irqflags_t irq=di();
    if(owner==p) {
        owner=0;queued=head=lost=closing=0;
        vm_input_configure(VM_POINTER_ABSOLUTE);
        mmio_write(MMIO_DISP_MODE,DISP_MODE_TTY);
    }
    irqrestore(irq);
}
void ripple_gfx_poll(void) {
    VmInputBatch batch; unsigned i; irqflags_t irq;
    if(!owner)return;
    irq=di();
    if(vm_input_read(incoming,8,&batch)==VM_DEVICE_OK) {
        if(batch.overflow)lost=1;
        for(i=0;i<batch.count;i++) {
            if(incoming[i].type==VM_INPUT_CLOSE){closing=1;continue;}
            if(queued==EVENTS){lost=1;continue;}
            events[(head+queued)%EVENTS]=incoming[i];++queued;
        }
    }
    irqrestore(irq);
    if(queued||lost||closing)ripple_gfx_wakeup();
}
int ripple_gfx_open(uint_fast8_t m,uint16_t f) { used(f);return m?fail(ENXIO):0; }
/* Closing inherited child descriptors must never destroy a parent's window.
 * Owner exit/exec also releases the lease even if a child retains an FD. */
int ripple_gfx_close(uint_fast8_t m) { used(m);return 0; }
int ripple_gfx_ioctl(uint_fast8_t m,uarg_t req,char *user) {
    struct gx_handle handle;
    unsigned row,n,bank,off,run,left;
    unsigned long from,to;
    irqflags_t irq;
    if(m)return fail(ENXIO);
    if(req==GX_INFO) {
        struct gx_info info={GX_VERSION,GX_CAP_COPY|GX_CAP_FILL|GX_CAP_INPUT,640,480,1,GX_RGB565,307200UL};
        return uput(&info,user,sizeof(info));
    }
    if(req==GX_CREATE) {
        struct gx_window w;VmDeviceCaps caps;VmInputState state;VmInputBatch batch;
        if(uget(user,&w,sizeof(w))||!valaddr_w(user,sizeof(w),sizeof(w)))return -1;
        if(w.version!=GX_VERSION||w.format!=GX_RGB565||!w.width||!w.height||w.width>640||w.height>480)return fail(EINVAL);
        if(owner)return fail(EBUSY);
        irq=di();
        if(vm_devices_caps(&caps)!=VM_DEVICE_OK || (caps.bits&(VM_CAP_RGB565_EXT|VM_CAP_INPUT_EVENTS|VM_CAP_INPUT_STATE|VM_CAP_INPUT_POLL))!=(VM_CAP_RGB565_EXT|VM_CAP_INPUT_EVENTS|VM_CAP_INPUT_STATE|VM_CAP_INPUT_POLL)) {irqrestore(irq);return fail(EOPNOTSUPP);}
        /* A new lease starts with an acknowledged snapshot and no old input. */
        vm_input_state(&state);
        for(n=0;n<32;n++) {
            if(vm_input_read(incoming,8,&batch)!=VM_DEVICE_OK||!batch.count)break;
        }
        owner=udata.u_ptab;owner_pid=owner->p_pid;width=w.width;height=w.height;
        if(!++generation)++generation;
        queued=head=lost=closing=0;
        mmio_write(MMIO_DISP_MODE,DISP_MODE_RGB565_EXT);
        irqrestore(irq);
        if(fill_rect(0,0,width,height,0)){ripple_gfx_exit(owner);return -1;}
        w.window=w.surface=generation;return uput(&w,user,sizeof(w));
    }
    /* All requests below begin with the same version and opaque handle. */
    if(uget(user,&handle,sizeof(handle)))return -1;
    if(handle.version!=GX_VERSION)return fail(EINVAL);
    if(mine(handle.handle))return -1;
    if(req==GX_WAIT) {
        struct gx_wait w;VmHostTime now;inoptr ino;unsigned device,ready_bits;
        if(uget(user,&w,sizeof(w))||!valaddr_w(user,sizeof(w),sizeof(w)))return -1;
        if(w.flags&~(GX_READY_INPUT|GX_READY_READ|GX_READY_WRITE)||w.timeout>600)return fail(EINVAL);
        ino=getinode(w.master);if(!ino)return -1;
        device=ino->c_node.i_addr[0];
        if(getmode(ino)!=MODE_R(F_CDEV)||(device>>8)!=3||(device&255)>=RIPPLE_PTY_COUNT)return fail(EINVAL);
        irq=di();waiting=1;
        if(w.timeout){udata.u_ptab->p_timeout=w.timeout+1;ptimer_insert();}
        for(;;) {
            ripple_gfx_poll();
            ready_bits=ripple_pty_status(device&255);
            if(queued||lost||closing)ready_bits|=GX_READY_INPUT;
            w.ready=ready_bits&w.flags;
            if(w.ready||!w.timeout||udata.u_ptab->p_timeout==1)break;
            if(psleep_flags(&waiting,0))break;
        }
        waiting=0;udata.u_ptab->p_timeout=0;irqrestore(irq);
        if(udata.u_error)return -1;
        irq=di();n=vm_host_monotonic(&now);irqrestore(irq);
        if(n!=VM_DEVICE_OK)return fail(EIO);
        w.milliseconds=now.seconds_low*1000UL+now.nanoseconds/1000000UL;
        return uput(&w,user,sizeof(w));
    }
    if(req==GX_DESTROY){ripple_gfx_exit(owner);return 0;}
    if(req==GX_PRESENT) {
        irq=di();n=vm_present_rgb565((void *)address(0),width,height,width);irqrestore(irq);
        return n==VM_DEVICE_OK?0:fail(EIO);
    }
    if(req==GX_FILL) {
        struct gx_fill f;if(uget(user,&f,sizeof(f)))return -1;
        if(!rect(f.x,f.y,f.width,f.height))return fail(EINVAL);
        return fill_rect(f.x,f.y,f.width,f.height,f.colour);
    }
    if(req==GX_COPY) {
        struct gx_copy c;if(uget(user,&c,sizeof(c)))return -1;
        if(!rect(c.sx,c.sy,c.width,c.height)||!rect(c.dx,c.dy,c.width,c.height))return fail(EINVAL);
        if(!c.width||!c.height)return 0;
        for(n=0;n<c.height;n++) {
            row=c.dy>c.sy?c.height-1-n:n;
            from=address((unsigned long)(c.sy+row)*width+c.sx);
            to=address((unsigned long)(c.dy+row)*width+c.dx);
            /* Host COPY snapshots each row, including horizontal overlap. */
            if(transfer(to,(unsigned)(from>>16),(unsigned)from,c.width,0,0))return -1;
        }
        return 0;
    }
    if(req==GX_UPLOAD) {
        struct gx_upload u;if(uget(user,&u,sizeof(u)))return -1;
        if(!rect(u.x,u.y,u.width,u.height)||u.stride<u.width)return fail(EINVAL);
        if(!u.width||!u.height)return 0;
        from=(unsigned long)ripple_userptr(u.pixels);
        if(!valaddr_r((void *)from,(unsigned long)(u.height-1)*u.stride+u.width,(unsigned long)(u.height-1)*u.stride+u.width))return -1;
        for(row=0;row<u.height;row++) {
            const uint16_t *src=(const uint16_t *)from+(unsigned long)row*u.stride;
            left=u.width;to=address((unsigned long)(u.y+row)*width+u.x);
            while(left) {
                if(ripple_mmu_source((unsigned long)src,&bank,&off,&run))return fail(EFAULT);
                n=left<run?left:run;
                if(transfer(to,bank,off,n,0,0))return -1;
                src+=n;to=address((unsigned long)(u.y+row)*width+u.x+u.width-left+n);left-=n;
            }
        }
        return 0;
    }
    if(req==GX_STATE) {
        struct gx_state s;VmInputState state;
        if(!valaddr_w(user,sizeof(s),sizeof(s)))return -1;
        irq=di();n=vm_input_state(&state);irqrestore(irq);if(n!=VM_DEVICE_OK)return fail(EIO);
        memset(&s,0,sizeof(s));s.version=GX_VERSION;s.window=generation;
        s.flags=state.flags;s.buttons=state.buttons;s.x=state.x;s.y=state.y;
        memcpy(s.held,state.held,sizeof(s.held));return uput(&s,user,sizeof(s));
    }
    if(req==GX_EVENT) {
        struct gx_event e;VmInputEvent event;
        if(!valaddr_w(user,sizeof(e),sizeof(e)))return -1;
        memset(&e,0,sizeof(e));e.version=GX_VERSION;e.window=generation;
        irq=di();
        if(closing){e.type=GX_CLOSE;closing=0;}
        else if(lost){e.type=GX_OVERFLOW;lost=queued=head=0;}
        else if(queued) {
            event=events[head];head=(head+1)%EVENTS;--queued;
            e.type=event.type;e.flags=event.flags;e.code=event.code;e.buttons=event.buttons;
            e.sequence=event.sequence;e.x=event.x;e.y=event.y;e.dx=event.dx;e.dy=event.dy;e.wheel_x=event.wheel_x;e.wheel_y=event.wheel_y;
        }
        irqrestore(irq);return uput(&e,user,sizeof(e));
    }
    return fail(EOPNOTSUPP);
}
