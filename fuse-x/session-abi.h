/* SPDX-License-Identifier: GPL-2.0-only */
/* Hosted adapter only. Process tables, authority and readiness stay in Riplix;
 * portable desktop state receives numeric execution identities, never p_tab. */
#include <gx-session.h>
#define GXS_HOST_TOUCH 64
#define GXS_HOST_EXIT 65
#define GXS_HOST_REVISION 66
static struct session_process {
    ptptr owner;
    unsigned pid;
    unsigned long id,seen;
    unsigned watching;
} session_processes[PTABSIZE];
static unsigned long session_serial,manager_serial,manager_token;
static unsigned long session_notified_revision;
static ptptr manager_owner;
static unsigned manager_pid;

static struct session_process *session_find(ptptr p,int create) {
    unsigned i;
    struct session_process *empty=NULL;
    for(i=0;i<PTABSIZE;i++) {
        if(session_processes[i].owner==p&&session_processes[i].pid==p->p_pid)return &session_processes[i];
        if(!session_processes[i].owner&&!empty)empty=&session_processes[i];
    }
    if(!create)return NULL;
    if(!empty||session_serial==0xffffffffUL){fail(ENOMEM);return NULL;}
    memset(empty,0,sizeof(*empty));empty->owner=p;empty->pid=p->p_pid;empty->id=++session_serial;
    return empty;
}
static void session_exit(ptptr p) {
    struct session_process *entry=session_find(p,0);
    unsigned long args[12]={0};
    if(manager_owner==p){manager_owner=NULL;manager_pid=0;manager_token=0;}
    if(!entry)return;
    args[0]=GXS_HOST_EXIT;args[1]=entry->id;
    rcc_hosted_gx(31,args);
    memset(entry,0,sizeof(*entry));
}
static unsigned long session_revision(void) {
    unsigned long args[12]={0};
    args[0]=GXS_HOST_REVISION;
    return rcc_hosted_gx(31,args)<0?0:args[4];
}
static int session_changed(struct session_process *entry,unsigned long revision) {
    return entry&&entry->watching&&revision&&entry->seen!=revision;
}
static int session_any_changed(void) {
    unsigned i;unsigned long revision=0;
    for(i=0;i<PTABSIZE;i++)if(session_processes[i].watching) {
        if(!revision)revision=session_revision();
        if(session_changed(&session_processes[i],revision)) {
            if(session_notified_revision==revision)return 0;
            session_notified_revision=revision;return 1;
        }
    }
    return 0;
}
static int session_own_changed(void) {
    struct session_process *entry=session_find(udata.u_ptab,0);
    return entry&&entry->watching&&session_changed(entry,session_revision());
}
static int session_ioctl(char *user) {
    struct gx_session request;
    struct session_process *entry,*child_entry;
    ptptr child;
    unsigned i,op;
    unsigned long args[12]={0},current,acknowledged;
    if(uget(user,&request,sizeof(request))||!valaddr_w(user,sizeof(request),sizeof(request)))return -1;
    if(request.version!=GXS_VERSION||request.reserved||request.flags)return fail(EINVAL);
    op=request.operation;
    acknowledged=request.revision;
    if(op>GXS_WATCH)return fail(EOPNOTSUPP);
    if(op==GXS_REGISTER||op==GXS_NEXT_WINDOW||op==GXS_GET_WINDOW||op==GXS_NEXT_INSTANCE||op==GXS_NEXT_PROCESS) {
        if(!valaddr_w((uint8_t *)request.record,sizeof(*request.record),sizeof(*request.record)))return -1;
        if(op==GXS_REGISTER&&!valaddr_r((const uint8_t *)request.record,sizeof(*request.record),sizeof(*request.record)))return -1;
    } else if(op==GXS_CONTROL&&(request.value==GXS_MOVE||request.value==GXS_RESIZE)) {
        if(!valaddr_r((const uint8_t *)request.record,sizeof(*request.record),sizeof(*request.record)))return -1;
    } else if(request.record)return fail(EINVAL);
    entry=session_find(udata.u_ptab,1);if(!entry)return -1;
    args[0]=op;args[1]=entry->id;args[2]=entry->pid;
    args[3]=request.object;args[4]=request.revision;args[5]=request.value;args[6]=(unsigned long)request.record;
    if(op==GXS_CLAIM_MANAGER) {
        if(udata.u_euid)return fail(EPERM);
        args[0]=GXS_INFO;if(host(31,args)<0)return -1;
        if(manager_owner&&(manager_owner!=udata.u_ptab||manager_pid!=udata.u_ptab->p_pid))return fail(EBUSY);
        if(!manager_owner&&manager_serial==0xffffffffUL)return fail(ENOMEM);
        args[0]=GXS_HOST_TOUCH;if(host(31,args)<0)return -1;
        if(!manager_owner){manager_owner=udata.u_ptab;manager_pid=entry->pid;manager_token=++manager_serial;}
        request.authority=manager_token;
    } else if(op==GXS_RELEASE_MANAGER) {
        if(!manager_token||request.authority!=manager_token||manager_owner!=udata.u_ptab||manager_pid!=entry->pid)return fail(EPERM);
        args[0]=GXS_INFO;if(host(31,args)<0)return -1;
        args[0]=64;if(host(32,args)<0)return -1;
        manager_owner=NULL;manager_pid=0;manager_token=0;request.authority=0;
    } else {
        if(op==GXS_CONTROL) {
            if(request.authority) {
                if(request.authority!=manager_token||manager_owner!=udata.u_ptab||manager_pid!=entry->pid)return fail(EPERM);
            } else if(!mine(request.object))return -1;
        } else if(request.authority)return fail(EINVAL);
        if(op==GXS_ATTACH_CHILD) {
            child=NULL;
            for(i=0;i<PTABSIZE;i++)if(ptab[i].p_status&&ptab[i].p_status!=P_ZOMBIE&&ptab[i].p_pid==request.object){child=&ptab[i];break;}
            if(!child)return fail(ESRCH);
            if(child->p_pptr!=udata.u_ptab||child==udata.u_ptab)return fail(EPERM);
            child_entry=session_find(child,1);if(!child_entry)return -1;
            args[8]=child_entry->id;args[9]=child_entry->pid;
        }
        if(host(31,args)<0)return -1;
    }
    request.object=args[3];request.revision=args[4];request.value=args[5];request.flags=args[7];
    if(op==GXS_WATCH) {
        /* The input revision is an acknowledgement, not a request to consume
         * whatever may have changed while the caller read its snapshot. */
        current=request.revision;entry->seen=acknowledged;entry->watching=1;
        request.flags=current!=entry->seen?GXS_CHANGED:0;
    }
    return uput(&request,user,sizeof(request));
}
