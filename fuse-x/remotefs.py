#!/usr/bin/env python3
"""Generate guest remote inode hooks from the existing external inode adapter."""
from pathlib import Path
import sys
source=Path(sys.argv[1]).read_text()
source=source[:source.index('static inoptr mount_directory')]
source=source.replace('#include "hostfs.h"','#include "remotefs-kernel.h"')
source=source.replace('extern long rcc_hosted_fs(unsigned, unsigned long *);','')
source=source.replace('char fuse_host_directory[1024];','')
source=source.replace('fuse_host_', 'fuse_remote_').replace('FUSE_HOST_DEV','FUSE_REMOTE_DEV')
source=source.replace('rcc_hosted_fs(op,a)','fuse_remote_request(op,a)')
source=source.replace('ino->c_dev==FUSE_REMOTE_DEV','fuse_remote_device(ino->c_dev)')
source=source.replace('static long request(unsigned op, unsigned long *a)', 'static long request(unsigned dev, unsigned op, unsigned long *a)')
source=source.replace('fuse_remote_request(op,a)', 'fuse_remote_request(dev,(a[7]=op,a))')
# Every request carries the owning mount; a blocked request cannot change a global selector.
import re
for function,dev in [('load','ino->c_dev'),('fuse_remote_node','dev'),('fuse_remote_search','parent->c_dev'),('fuse_remote_create','parent->c_dev'),('fuse_remote_link','parent->c_dev'),('fuse_remote_empty','ino->c_dev'),('fuse_remote_read','ino->c_dev'),('fuse_remote_write','ino->c_dev'),('fuse_remote_open','ino->c_dev'),('fuse_remote_truncate','ino->c_dev'),('fuse_remote_chmod','ino->c_dev'),('fuse_remote_store','ino->c_dev'),('fuse_remote_deref','ino->c_dev'),('fuse_remote_rename','sp->c_dev')]:
    match=re.search(r'\b'+function+r'\([^;]*?\) \{',source)
    if not match:raise ValueError(function)
    start=match.end();end=source.index('\n}',start)
    region=source[start:end]
    region=region.replace('request(', 'request('+dev+',')
    region=region.replace('fuse_remote_node(', 'fuse_remote_node('+dev+',')
    source=source[:start]+region+source[end:]
def once(old,new):
    global source
    if source.count(old)!=1:raise ValueError('remote inode hook contract changed: '+old)
    source=source.replace(old,new)
once('inoptr fuse_remote_node(unsigned number)', 'inoptr fuse_remote_node(unsigned dev,unsigned number)')
once('fuse_remote_inode(ino) && ino->c_num==number','ino->c_dev==dev && ino->c_num==number')
once('free->c_dev=FUSE_REMOTE_DEV;', 'free->c_dev=dev;')
once('free->c_super=1;', 'free->c_super=fs_tab_get(dev)-fs_tab;')
once('int fuse_remote_sync(void) {unsigned long a[8]={0};return request(13,a)<0?-1:0;}',
                      'int fuse_remote_sync_dev(unsigned dev) {unsigned long a[8]={0};return request(dev,13,a)<0?-1:0;}')
once('free->c_num=number;', 'free->c_refs=1;free->c_num=number;')
once('if(fuse_remote_refresh(ino)<0)return 0;\n            return i_ref(ino);',
    'i_ref(ino);if(fuse_remote_refresh(ino)<0){fuse_remote_deref(ino);return 0;}\n            return ino;')
once('else if(src->c_num==ROOTINODE', 'else if(sp->c_dev!=dp->c_dev)udata.u_error=EXDEV;\n        else if(src->c_num==ROOTINODE')
once('if(!--ino->c_refs) {a[0]=ino->c_num;request(ino->c_dev,12,a);memset(ino,0,sizeof(*ino));}',
    'if(ino->c_refs==1){a[0]=ino->c_num;request(ino->c_dev,12,a);}\n    if(!--ino->c_refs)memset(ino,0,sizeof(*ino));')
once('a[1]=O_ACCMODE(flags)!=O_RDONLY;', 'a[1]=O_ACCMODE(flags);a[2]=!!(flags&O_APPEND);')
source+='\ninoptr fuse_remote_node_dev(unsigned dev,unsigned number) {return fuse_remote_node(dev,number); }\n'
Path(sys.argv[2]).write_text(source)
