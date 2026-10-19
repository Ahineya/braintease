/* SPDX-License-Identifier: GPL-2.0-only */
/* Small libc adapter for the upstream uname utility; data comes from _uname. */
#include "user.h"
#include "sysinfoblk.h"
#include <sys/utsname.h>
static int field(char *dest,unsigned capacity,const char **source,const char *end) {
    unsigned i=0;
    while (*source<end && **source) {
        if (i+1>=capacity) return -1;
        dest[i++]=*(*source)++;
    }
    if (*source==end) return -1;
    ++*source; dest[i]=0; return 0;
}
int uname(struct utsname *out) {
    struct sysinfoblk info;
    const char *text=info.uname,*end=info.uname+sizeof(info.uname);
    long size=call(54,(long)&info,sizeof(info),0,0);
    if (size!=sizeof(info)) return -1;
    if (field(out->release,sizeof(out->release),&text,end) ||
        field(out->sysname,sizeof(out->sysname),&text,end) ||
        field(out->nodename,sizeof(out->nodename),&text,end) ||
        field(out->machine,sizeof(out->machine),&text,end)) return -1;
    /* Keep Ripple as the machine ISA; identify this build in the version. */
    memcpy(out->version,"RCC-BF",7);
    return 0;
}
