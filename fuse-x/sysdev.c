/* SPDX-License-Identifier: GPL-2.0-only */
/* Hosted standard system devices and the shared port's process-report ABI. */
#include "host.h"
#include <kdata.h>
#include <string.h>
#include "resources.h"
int fuse_system_open(uint_fast8_t m,uint16_t flags) {
    used(flags);
    if(m==0 || m==2 || m==3) return 0;
    udata.u_error=ENXIO;return -1;
}
int fuse_system_read(uint_fast8_t m,uint_fast8_t raw,uint_fast8_t flags) {
    usize_t n=udata.u_count>32767UL ? 32767UL : udata.u_count;
    used(raw);used(flags);
    if(!m) return 0;
    if(m==2) {
        if(udata.u_sysio) memset(udata.u_base,0,n);
        else if(uzero(udata.u_base,n)) return -1;
        return n;
    }
    if(m==3) {
        if(udata.u_offset<0 || udata.u_offset>=sizeof(ptab)) return 0;
        if(n>sizeof(struct p_tab)) n=sizeof(struct p_tab);
        if(n>sizeof(ptab)-udata.u_offset) n=sizeof(ptab)-udata.u_offset;
        if(uput((char *)ptab+udata.u_offset,udata.u_base,n)) return -1;
        return umove(n);
    }
    udata.u_error=ENXIO;return -1;
}
int fuse_system_write(uint_fast8_t m,uint_fast8_t raw,uint_fast8_t flags) {
    used(raw);used(flags);
    if(m==0 || m==2) return udata.u_count>32767UL ? 32767 : (int)udata.u_count;
    udata.u_error=EINVAL;return -1;
}
int fuse_system_ioctl(uint_fast8_t m,uarg_t request,char *p) {
    if(m==3) {
        if(request==RIPPLE_PROCINFO) {
            struct ripple_procinfo info;
            unsigned slot;
            ptptr task;
            if(uget(p,&slot,sizeof(slot)) || !valaddr_w(p,sizeof(info),sizeof(info))) return -1;
            if(slot>=PTABSIZE) {udata.u_error=EINVAL;return -1;}
            memset(&info,0,sizeof(info));
            sync_clock();
            info.slot=slot;info.version=RIPPLE_PROCINFO_VERSION;info.cpu_hz=TICKSPERSEC;
            info.sample_ticks=ticks.full*(TICKSPERSEC/10)+ticks_this_dsecond;
            task=&ptab[slot];
            if(task->p_status) {
                info.pid=task->p_pid;info.ppid=task->p_pptr->p_pid;
                info.pgrp=task->p_pgrp;info.session=task->p_session;
                info.uid=task->p_uid;info.tty=task->p_tty;info.state=task->p_status;
                info.priority=task->p_priority;info.nice=task->p_nice;
                info.start_ticks=task->p_time*(TICKSPERSEC/10);
                info.user_ticks=task->p_utime;info.system_ticks=task->p_stime;
                memcpy(info.name,task->p_name,8);
                /* Same provisional accounting as pagemap_mem_used(). */
                if(task->p_status!=P_ZOMBIE) info.private_kib=task->p_size;
                info.mapped_kib=info.private_kib;
            }
            return uput(&info,p,sizeof(info));
        }
        if(request==RIPPLE_SYSINFO) {
            struct ripple_sysinfo info;
            unsigned i;
            if(!valaddr_w(p,sizeof(info),sizeof(info))) return -1;
            memset(&info,0,sizeof(info));sync_clock();
            info.version=RIPPLE_SYSINFO_VERSION;
            info.ram_kib=ramsize;info.pool_kib=procmem;info.used_kib=pagemap_mem_used();
            info.mapped_kib=info.used_kib;info.slots=PTABSIZE;
            info.heap_cells=16UL*64000UL;info.stack_cells=3UL*64000UL;
            info.uptime_ticks=ticks.full;info.ticks_per_second=10;
            for(i=0;i<PTABSIZE;i++) if(ptab[i].p_status!=P_EMPTY) ++info.processes;
            return uput(&info,p,sizeof(info));
        }
        if(request==1) return uputi(PTABSIZE,p);
        if(request==2) return uputi(sizeof(struct p_tab),p);
    }
    udata.u_error=ENOTTY;return -1;
}
