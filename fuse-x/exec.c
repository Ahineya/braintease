/* SPDX-License-Identifier: GPL-2.0-only */
#include "host.h"
#include "pcm.h"
#include "hostfs.h"
#include "graphics.h"
#include <kdata.h>
#include <string.h>
#include <native_hosted.h>
#include <native_process.h>

extern void fuse_init_entry(unsigned long);
extern void fuse_shell_entry(unsigned long);
extern void fuse_hello_entry(unsigned long);
extern void fuse_test_entry(unsigned long);
struct native_program { const char *manifest; void (*entry)(unsigned long); };
static const struct native_program programs[] = {
    {"FXN1:init:1\n", fuse_init_entry},
    {"FXN1:sh:1\n", fuse_shell_entry},
    {"FXN1:hello:1\n", fuse_hello_entry},
    {"FXN1:check:1\n", fuse_test_entry}
};
struct native_arguments { unsigned argc; char **argv, **envp; };

/* Validate before doing logical pointer arithmetic, including malformed argv
 * pointers. The portable rargs routine assumes a machine fault can recover an
 * invalid access; a hosted runtime fault would instead stop the session. */
static bool native_args(char **user, struct s_argblk *args) {
    char *write=(char *)args->a_buf, *end=write+sizeof(args->a_buf);
    args->a_argc=0;
    for (;;) {
        char *text;
        if (!valaddr_r((void *)user,sizeof(uptr_t),sizeof(uptr_t))) return true;
        text=(void *)_ugetl((void *)user);
        if (!text) break;
        user=(void *)((char *)user+sizeof(uptr_t));
        ++args->a_argc;
        for (;;) {
            int c;
            if (write==end) {udata.u_error=E2BIG;return true;}
            if (!valaddr_r((void *)text,1,1)) return true;
            c=_ugetc((void *)text);
            *write++=c;
            if (!c) break;
            ++text;
        }
    }
    args->a_arglen=ALIGNUP(write-(char *)args->a_buf);
    return false;
}

/* Bootstrap manifests and independently compiled native packages share the
 * kernel's path, permission, argument, descriptor and process setup. */
arg_t _execve(void) {
    inoptr ino = n_open_argn();
    struct s_argblk *args, *env;
    struct s_argblk saved_args, saved_env;
    struct native_arguments *context = (void *)(PROGBASE + 16);
    char manifest[32];
    unsigned i, program = 65535U;
    int argc;
    unsigned long entry=0, slice=0;
    void *package=NULL;
    char **argv, **envp;
    if (!ino) return -1;
    if (getmode(ino) != MODE_R(F_REG) || !(getperm(ino) & OTH_EX) ||
        (fs_tab[ino->c_super].m_flags & MS_NOEXEC)) { udata.u_error=EACCES; goto bad; }
    if (ino->c_node.i_size < sizeof(manifest)) {
        memset(manifest,0,sizeof(manifest));
        udata.u_offset=0; udata.u_base=(void *)manifest;
        udata.u_count=ino->c_node.i_size; udata.u_sysio=1;
        readi(ino,0);
        if (udata.u_done != ino->c_node.i_size) { udata.u_error=EIO; goto bad; }
        for (i=0;i<sizeof(programs)/sizeof(programs[0]);i++)
            if (ino->c_node.i_size == strlen(programs[i].manifest) && !strcmp(manifest,programs[i].manifest)) program=i;
        if (program==65535U) { udata.u_error=ENOEXEC; goto bad; }
    } else {
        if (ino->c_node.i_size>67108864UL) {udata.u_error=ENOEXEC;goto bad;}
        /* A Bourne shell retries ordinary executable text as a script after
         * ENOEXEC. Reject it quietly before invoking the native loader. */
        udata.u_offset=0;udata.u_base=(void *)manifest;
        udata.u_count=8;udata.u_sysio=1;readi(ino,0);
        if (udata.u_done!=8) {udata.u_error=EIO;goto bad;}
        if (memcmp(manifest,"FUSEX001",8)) {udata.u_error=ENOEXEC;goto bad;}
        package=rcc_process_kernel_alloc(4096);
        if (!package) {udata.u_error=ENOMEM;goto bad;}
        if (!rcc_hosted_image_begin(ino->c_node.i_size)) {udata.u_error=ENOMEM;goto bad;}
        udata.u_offset=0;
        while (udata.u_offset<ino->c_node.i_size) {
            unsigned long count=ino->c_node.i_size-udata.u_offset;
            if (count>4096) count=4096;
            udata.u_base=package;udata.u_count=count;udata.u_sysio=1;
            readi(ino,0);
            if (udata.u_done!=count) {udata.u_error=EIO;goto bad;}
            if (!rcc_hosted_image_append(package,count)) {udata.u_error=ENOEXEC;goto bad;}
            fuse_exec_yield(&slice,0);
        }
        rcc_process_kernel_free(package);package=NULL;
        if (!rcc_hosted_image_prepare()) {udata.u_error=ENOMEM;goto bad;}
        do {
            entry=rcc_hosted_image_poll();
            if(entry==0xFFFFFFFFUL) {entry=0;udata.u_error=ENOEXEC;goto bad;}
            if(!entry) fuse_exec_yield(&slice,1);
        } while(!entry);
    }
    args=&saved_args; env=&saved_env;
    if (native_args((void *)udata.u_argn1,args) || native_args((void *)udata.u_argn2,env)) {
        goto bad;
    }
    if ((unsigned long)args->a_arglen + env->a_arglen +
        (args->a_argc + env->a_argc + 2UL) * sizeof(uptr_t) > RIPPLE_ARG_MAX) {
        udata.u_error=E2BIG; goto bad;
    }
    memset((void *)PROGBASE,0,64000);
    argv=(char **)wargs((void *)PROGTOP,args,&argc);
    envp=(char **)wargs((void *)argv,env,NULL);
    ripple_gfx_exit(udata.u_ptab);
    ripple_pcm_exit(udata.u_ptab);
    for (i=0;i<UFTSIZE;i++) if (udata.u_cloexec & (1UL<<i)) doclose(i);
    udata.u_cloexec=0;
    for (i=0;i<sizeof(udata.u_sigvec)/sizeof(udata.u_sigvec[0]);i++)
        if (udata.u_sigvec[i]!=SIG_IGN) udata.u_sigvec[i]=SIG_DFL;
    udata.u_top=PROGTOP; udata.u_ptab->p_top=PROGTOP;
    /* Old SDK allocators retain their bounded arena. New SDK allocators opt
     * into the extended heap on their first growth, without a package ABI bump. */
    udata.u_codebase=PROGBASE; udata.u_break=PROGBASE+256;
    udata.u_error=udata.u_cursig=0;
    udata.u_sigtramp=0;
    memset(udata.u_name,0,sizeof(udata.u_name));
    if (argc) {
        char *name=(char *)ugetp(argv);
        for(i=0;i<sizeof(udata.u_name);i++) {
            int c=ugetc(name+i);
            if(c<0) break;
            udata.u_name[i]=c;
            if(!c) break;
        }
    }
    memcpy(udata.u_ptab->p_name,udata.u_name,sizeof(udata.u_name));
    udata.u_ptab->p_flags|=PFL_EXECED;
    i_deref(ino);
    context->argc=argc; context->argv=argv; context->envp=envp;
    udata.u_insys=0;
    proc_sched_switch();
    rcc_hosted_exec(entry ? (void (*)(unsigned long))entry : programs[program].entry,(unsigned long)context);
    panic("Fuse-X: exec returned");
bad:
    rcc_hosted_image_cancel();
    if (package) rcc_process_kernel_free(package);
    if (entry) rcc_hosted_image_discard(entry);
    i_deref(ino);
    return -1;
}
