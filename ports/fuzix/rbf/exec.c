/* SPDX-License-Identifier: GPL-2.0-only */
#include <kernel.h>
#include <kdata.h>
#include <printf.h>
#include <vm_bulk.h>

/* Fixed SYS1 slots: init in code banks 10..11; child programs in 12..13.
 * Arguments live at the top of the user stack bank, outside its growing stack. */
#define ARG_CELLS 1024
#define MAX_ARGS 24
static uint8_t stream[1024], strings[ARG_CELLS];
static unsigned offsets[MAX_ARGS], string_count;
unsigned rbf_argc, rbf_argv;
static inoptr executable;
extern void rbf_enter(unsigned, unsigned);
static int fetch(unsigned long offset, unsigned count) {
    udata.u_sysio=1; udata.u_error=0; udata.u_done=0;
    udata.u_offset=offset; udata.u_base=stream; udata.u_count=count;
    readi(executable,0);
    if (udata.u_error || udata.u_done!=count) { udata.u_error=EIO; return -1; }
    return 0;
}
static unsigned word(unsigned off) {
    return (stream[off]&255) | ((stream[off+1]&255)<<8);
}
static unsigned long dword(unsigned off) {
    return word(off) | ((unsigned long)word(off+2)<<16);
}
static int transfer(unsigned bank, unsigned offset, unsigned count, unsigned format) {
    unsigned long source=(unsigned long)stream;
    if (vm_bulk_copy(source>>16,(unsigned)source,bank,offset,count,format)!=VM_DEVICE_OK) {
        udata.u_error=EIO; return -1;
    }
    return 0;
}
static int stage_arguments(unsigned long vector,unsigned long env) {
    unsigned long ptr;
    int c;
    rbf_argc=string_count=0;
    /* Environment support is explicit rather than silently dropping values. */
    if (env) {
        ptr=ugetp((void *)env);
        if (udata.u_error) return -1;
        if (ptr) { udata.u_error=EOPNOTSUPP; return -1; }
    }
    if (!vector) return 0;
    for (;;) {
        ptr=ugetp((void *)(vector+rbf_argc*sizeof(uptr_t)));
        if (udata.u_error) return -1;
        if (!ptr) break;
        if (rbf_argc==MAX_ARGS) { udata.u_error=E2BIG; return -1; }
        offsets[rbf_argc++]=string_count;
        do {
            if (string_count+(rbf_argc+1)*sizeof(uptr_t)>=ARG_CELLS) {
                udata.u_error=E2BIG; return -1;
            }
            c=ugetc((void *)ptr);
            if (udata.u_error) return -1;
            strings[string_count++]=c;
            ++ptr;
        } while (c);
    }
    return 0;
}
static int fail(unsigned error) {
    i_deref(executable);
    udata.u_error=error;
    return -1;
}
/* All recoverable checks finish before replacing RAM or the process's code.
 * A device failure after commit kills only the child; it cannot return into a
 * partly overwritten executable. The saved parent's code uses another slot. */
static void committed_failure(void) {
    if (udata.u_ptab->p_pid==1) panic("init transfer");
    doexit(SIGKILL);
    panic("exec failure returned");
}
static int load(unsigned base) {
    unsigned long entry,nins,data,remaining,position,at,ram,code;
    unsigned n,bank,off,i,vector_cells;
    if (getmode(executable)!=MODE_R(F_REG) ||
        !(executable->c_node.i_mode&0111)) return fail(EACCES);
    if (executable->c_node.i_size<24 || fetch(0,24)) return fail(ENOEXEC);
    if (word(0)!=0x5953 || word(2)!=0x3153 || word(4)!=BANK_SIZE ||
        word(6)!=base || word(8)!=3 || word(10)!=4) return fail(ENOEXEC);
    entry=dword(12); nins=dword(16); data=dword(20);
    if (!nins || nins>RBF_IMAGE_BANKS*1UL*BANK_SIZE || entry>=nins || data>BANK_SIZE ||
        nins>(executable->c_node.i_size-24)/8 ||
        executable->c_node.i_size-24-nins*8!=data) return fail(ENOEXEC);
    if (vm_memory_info(&ram,&code)!=VM_DEVICE_OK || ram<RBF_RAM_BANKS*1UL*BANK_SIZE ||
        code<base*1UL*BANK_SIZE+nins) return fail(ENOMEM);
    position=24; at=0; remaining=nins;
    while (remaining) {
        bank=base+at/BANK_SIZE; off=at%BANK_SIZE;
        n=remaining>128?128:(unsigned)remaining;
        if (n>BANK_SIZE-off) n=BANK_SIZE-off;
        if (fetch(position,n*8) || transfer(bank,off,n,VM_BULK_CODE_OCTETS)) committed_failure();
        position+=n*8; at+=n; remaining-=n;
    }
    if (vm_bulk_fill(3,0,BANK_SIZE,0)!=VM_DEVICE_OK ||
        vm_bulk_fill(4,0,BANK_SIZE,0)!=VM_DEVICE_OK) committed_failure();
    at=0;
    while (at<data) {
        n=data-at>sizeof(stream)?sizeof(stream):(unsigned)(data-at);
        if (fetch(position,n) || transfer(3,(unsigned)at,n,VM_BULK_OCTETS)) committed_failure();
        at+=n; position+=n;
    }
    i_deref(executable);
    rbf_argv=BANK_SIZE-ARG_CELLS;
    vector_cells=(rbf_argc+1)*sizeof(uptr_t);
    uput(strings,(void *)(0x40000UL+rbf_argv+vector_cells),string_count);
    for (i=0;i<rbf_argc;++i)
        uputp(0x40000UL+rbf_argv+vector_cells+offsets[i],(void *)(0x40000UL+rbf_argv+i*sizeof(uptr_t)));
    uputp(0,(void *)(0x40000UL+rbf_argv+rbf_argc*sizeof(uptr_t)));
    for (i=0;i<UFTSIZE;++i) if (udata.u_cloexec&(1U<<i)) doclose(i);
    udata.u_cloexec=0;
    udata.u_sysio=0; udata.u_error=0; udata.u_insys=0;
    udata.u_top=PROGTOP; udata.u_ptab->p_top=PROGTOP;
    if (base==RBF_INIT_CODE) kputs("Starting /init (physical memory, IRQ off).\n");
    rbf_enter(base+entry/BANK_SIZE,entry%BANK_SIZE);
    panic("exec returned");
    return -1;
}
int rbf_execve(void) {
    unsigned long path=udata.u_argn,vector=udata.u_argn1,env=udata.u_argn2;
    executable=n_open((uint8_t *)path,NULLINOPTR);
    if (!executable) return -1;
    udata.u_error=0;
    if (stage_arguments(vector,env)) return fail(udata.u_error);
    return load(udata.u_ptab->p_pid==1?RBF_INIT_CODE:RBF_APP_CODE);
}
void rbf_exec_init(void) {
    udata.u_error=0;
    uput("/init",(void *)PROGBASE,6);
    executable=n_open((uint8_t *)PROGBASE,NULLINOPTR);
    if (!executable) panic("init missing");
    rbf_argc=1; offsets[0]=0; string_count=6;
    memcpy(strings,"/init",6);
    if (load(RBF_INIT_CODE)) panic("init executable");
}
