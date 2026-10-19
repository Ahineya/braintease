/* SPDX-License-Identifier: GPL-2.0-only */
/* Bounded C16 backing store for NetSurf's HTTP cache. Policy stays in llcache.
 * Each committed slot contains metadata AND body; rename publishes them together.
 * One process owns a cache directory. In-use buffers survive invalidation and
 * eviction until llcache releases them, including failed persistence attempts. */
#include <netsurf_profile.h>
#include <stdbool.h>
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/file.h>
#include "utils/nsurl.h"
#include "c16-cache.h"
#define SLOTS 128
#define BODY_LIMIT 262144UL
#define META_LIMIT 24576UL
#define MAGIC 0x4e534332UL
struct record { unsigned long magic, hash, body, meta, sum; };
struct slot {
    struct record disk;
    char *url;
    ns_octet_t *buffer[2];
    unsigned refs[2];
    unsigned long used;
    bool valid, pending;
};
static struct slot slots[SLOTS];
static char directory[240];
static int lock_fd=-1;
/* Metadata is fetched/released immediately before source data. Keep at most
 * one verified, unreferenced body warm for that handoff, avoiding a second
 * disk read and checksum without accumulating an unaccounted RAM cache. */
static int warm=-1;
static unsigned long total, limit, serial;
static bool warned;
static void warning(const char *reason)
{
    if(!warned) { fprintf(stderr,"netsurf: disk cache %s; browsing continues\n",reason);warned=true; }
}
bool c16_cache_enabled(void) { return lock_fd>=0; }
const char *c16_cache_default_path(void)
{
    static char path[240];
    const char *home=getenv("HOME");
    if(!home||!*home)home="/";
    if(strlen(home)>200)return NULL;
    snprintf(path,sizeof(path),"%s%s.netsurf-cache",home,home[strlen(home)-1]=='/'?"":"/");
    return path;
}
static void filename(char *path,unsigned i,bool temporary)
{ snprintf(path,256,"%s/%03u.%s",directory,i,temporary?"tmp":"cache"); }
static bool io_all(int fd,void *buffer,unsigned long length,bool writing)
{
    ns_octet_t *p=buffer;
    while(length) {
        unsigned chunk=length>4096?4096:(unsigned)length;
        ssize_t n=writing?write(fd,p,chunk):read(fd,p,chunk);
        if(n<0&&errno==EINTR)continue;
        if(n<=0)return false;
        p+=n;length-=n;
    }
    return true;
}
/* Adler-32, reducing per block to keep C16's explicitly wide sums bounded. */
static unsigned long checksum(const ns_octet_t *p,unsigned long n,unsigned long sum)
{
    unsigned long a=sum&65535UL,b=sum>>16;
    while(n) {
        unsigned block=n>4096?4096:(unsigned)n,i;
        for(i=0;i<block;i++) { a+=*p++;b+=a; }
        a%=65521UL;b%=65521UL;n-=block;
    }
    return (b<<16)|a;
}
static void forget(unsigned i)
{
    struct slot *s=&slots[i];char path[256];
    if(warm==(int)i) {
        if(!s->refs[0]) { free(s->buffer[0]);s->buffer[0]=NULL; }
        warm=-1;
    }
    if(s->valid) { total-=sizeof(struct record)+s->disk.body+s->disk.meta; s->valid=false; }
    filename(path,i,false);unlink(path);
    filename(path,i,true);unlink(path);
    s->pending=false;
    if(!s->refs[0]&&!s->refs[1]) { free(s->url);s->url=NULL; }
}
static bool idle(unsigned i) { return !slots[i].refs[0]&&!slots[i].refs[1]; }
static void cool(int except)
{
    if(warm>=0&&warm!=except) {
        struct slot *s=&slots[warm];
        if(!s->refs[0]) { free(s->buffer[0]);s->buffer[0]=NULL; }
        warm=-1;
    }
}
static int oldest(int except,bool empty_ok)
{
    unsigned i;int best=-1;
    for(i=0;i<SLOTS;i++)if((int)i!=except&&idle(i)) {
        if(!slots[i].valid) { if(empty_ok)return i;continue; }
        if(best<0||slots[i].used<slots[best].used)best=i;
    }
    return best;
}
static bool number(const char *p,unsigned long *value)
{
    char *end;unsigned long n;
    if(*p<'0'||*p>'9')return false;
    errno=0;n=strtoul(p,&end,10);
    if(errno||*end)return false;
    *value=n;return true;
}
/* Validate every string before upstream's strlen-based metadata reader sees it.
 * Our worker does not export DER chains; reject other metadata dialects. */
static bool metadata_ok(const ns_octet_t *data,unsigned long n,unsigned long body,const char *url)
{
    unsigned long pos=0,v,headers=0;unsigned line=0;
    while(pos<n) {
        const char *p=(const char *)data+pos;
        const ns_octet_t *end=memchr(data+pos,0,n-pos);
        if(!end)return false;
        if(line==0) { if(url&&strcmp(p,url))return false; }
        else if(line<=5) {
            if(!number(p,&v))return false;
            if(line==1&&v!=body)return false;
            if(line==5) { if(v>1024)return false;headers=v; }
        } else if(line==6+headers) {
            if(strcmp(p,"0")||end!=data+n-1)return false;
        }
        pos=(unsigned long)(end-data)+1;line++;
    }
    return line==7+headers;
}
static int find(nsurl *url)
{
    unsigned i;unsigned long hash=nsurl_hash(url);const char *key=nsurl_access(url);
    for(i=0;i<SLOTS;i++)if(slots[i].disk.hash==hash&&(slots[i].valid||slots[i].url)) {
        struct slot *s=&slots[i];
        if(!s->url&&s->valid) {
            char path[256];ns_octet_t *meta=malloc(s->disk.meta);int fd;
            if(!meta)return -1;
            filename(path,i,false);fd=open(path,O_RDONLY);
            if(fd>=0&&lseek(fd,sizeof(struct record),SEEK_SET)>=0&&io_all(fd,meta,s->disk.meta,false)&&
               metadata_ok(meta,s->disk.meta,s->disk.body,NULL))s->url=strdup((char *)meta);
            if(fd>=0)close(fd);
            free(meta);
            if(!s->url) { forget(i);continue; }
        }
        if(s->url&&!strcmp(s->url,key))return i;
    }
    return -1;
}
static nserror initialise(const struct llcache_store_parameters *parameters)
{
    unsigned i;char path[256];struct stat st;
    if(!parameters->path||!parameters->limit)return NSERROR_OK;
    if(lock_fd>=0)return NSERROR_INIT_FAILED;
    warned=false;
    if(strlen(parameters->path)>=sizeof(directory)) { warning("path is too long");return NSERROR_OK; }
    strcpy(directory,parameters->path);
    if((mkdir(directory,0700)<0&&errno!=EEXIST)||stat(directory,&st)<0||!S_ISDIR(st.st_mode)) {
        warning("directory is unavailable");return NSERROR_OK;
    }
    snprintf(path,sizeof(path),"%s/lock",directory);lock_fd=open(path,O_RDWR|O_CREAT,0600);
    if(lock_fd<0||flock(lock_fd,LOCK_EX|LOCK_NB)<0) {
        if(lock_fd>=0)close(lock_fd);lock_fd=-1;warning("is unavailable or in use by another browser");return NSERROR_OK;
    }
    fcntl(lock_fd,F_SETFD,FD_CLOEXEC);
    memset(slots,0,sizeof(slots));total=serial=0;
    limit=parameters->limit>C16_CACHE_LIMIT?C16_CACHE_LIMIT:parameters->limit;
    for(i=0;i<SLOTS;i++) {
        struct slot *s=&slots[i];int fd;
        filename(path,i,true);unlink(path); /* Incomplete previous write. */
        filename(path,i,false);fd=open(path,O_RDONLY);
        if(fd<0)continue;
        if(io_all(fd,&s->disk,sizeof(struct record),false)&&fstat(fd,&st)==0&&S_ISREG(st.st_mode)&&
           s->disk.magic==MAGIC&&s->disk.body<=BODY_LIMIT&&s->disk.meta>0&&s->disk.meta<=META_LIMIT&&
           (unsigned long)st.st_size==sizeof(struct record)+s->disk.body+s->disk.meta) {
            s->valid=true;s->used=(unsigned long)st.st_mtime;
            if(s->used>serial)serial=s->used;
            total+=st.st_size;
        } else unlink(path);
        close(fd);
    }
    while(total>limit) { int i=oldest(-1,false);if(i<0)break;forget(i); }
    return NSERROR_OK;
}
static nserror release(nsurl *url,enum backing_store_flags flags)
{
    int i=find(url);unsigned kind=flags==BACKING_STORE_META?1:0;struct slot *s;
    if(i<0)return NSERROR_NOT_FOUND;
    s=&slots[i];
    if(!s->refs[kind])return NSERROR_NOT_FOUND;
    if(!--s->refs[kind]) { free(s->buffer[kind]);s->buffer[kind]=NULL; }
    if(!s->valid&&idle(i)) { free(s->url);s->url=NULL;s->pending=false; }
    return NSERROR_OK;
}
static nserror invalidate(nsurl *url)
{ int i=find(url);if(i<0)return NSERROR_NOT_FOUND;forget(i);return NSERROR_OK; }
static nserror store(nsurl *url,enum backing_store_flags flags,ns_octet_t *data,const size_t length)
{
    int i;struct slot *s;char path[256],temp[256];
    if(lock_fd<0)return NSERROR_SAVE_FAILED;
    cool(-1);
    i=find(url);
    if(flags==BACKING_STORE_NONE) {
        /* llcache never releases a NULL source buffer; leave empty objects in
         * RAM rather than permanently pinning a slot without a release call. */
        if(!length||length>BODY_LIMIT||!data)return NSERROR_SAVE_FAILED;
        /* The key-only release API cannot distinguish generations. Never
         * overwrite an entry while an earlier llcache object holds it. */
        if(i>=0&&!idle(i))return NSERROR_SAVE_FAILED;
        if(i<0)i=oldest(-1,true);
        if(i<0)return NSERROR_SAVE_FAILED;
        s=&slots[i];forget(i);
        s->url=strdup(nsurl_access(url));if(!s->url)return NSERROR_NOMEM;
        s->disk.hash=nsurl_hash(url);s->disk.body=length;
        s->buffer[0]=data;s->refs[0]=1;s->pending=true;
        return NSERROR_OK;
    }
    if(i<0||!slots[i].pending)return NSERROR_SAVE_FAILED;
    s=&slots[i];
    if(!data||!length||length>META_LIMIT||!metadata_ok(data,length,s->disk.body,s->url))return NSERROR_SAVE_FAILED;
    s->disk.magic=MAGIC;s->disk.meta=length;
    if(sizeof(struct record)+s->disk.body+length>limit)return NSERROR_SAVE_FAILED;
    while(total+sizeof(struct record)+s->disk.body+length>limit) {
        int victim=oldest(i,false);if(victim<0)return NSERROR_SAVE_FAILED;
        forget(victim);
    }
    s->disk.sum=checksum(s->buffer[0],s->disk.body,checksum(data,length,1));
    filename(path,i,false);filename(temp,i,true);
    {
        int fd=open(temp,O_WRONLY|O_CREAT|O_TRUNC,0600);bool ok;
        if(fd<0) { warning("write failed");return NSERROR_SAVE_FAILED; }
        ok=io_all(fd,&s->disk,sizeof(struct record),true)&&io_all(fd,data,length,true)&&
            io_all(fd,s->buffer[0],s->disk.body,true);
        if(close(fd)<0)ok=false;
        if(!ok||rename(temp,path)<0) { unlink(temp);warning("write failed");return NSERROR_SAVE_FAILED; }
    }
    s->buffer[1]=data;s->refs[1]=1;s->pending=false;s->valid=true;s->used=++serial;
    total+=sizeof(struct record)+s->disk.body+length;
    return NSERROR_OK;
}
static nserror fetch(nsurl *url,enum backing_store_flags flags,ns_octet_t **data,size_t *length)
{
    int i,fd;struct slot *s;char path[256];struct record disk;
    ns_octet_t *meta=NULL,*body=NULL;bool ok;
    unsigned kind=flags==BACKING_STORE_META?1:0;
    if(lock_fd<0)return NSERROR_NOT_FOUND;
    i=find(url);if(i<0||!slots[i].valid)return NSERROR_NOT_FOUND;
    cool(i);
    s=&slots[i];s->used=++serial;
    if(!s->buffer[kind]) {
        /* Verify the pair, including its checksum, before returning metadata. */
        meta=malloc(s->disk.meta);body=malloc(s->disk.body?s->disk.body:1);
        if(!meta||!body) { free(meta);free(body);return NSERROR_NOMEM; }
        filename(path,i,false);fd=open(path,O_RDONLY);
        ok=fd>=0&&io_all(fd,&disk,sizeof(disk),false)&&!memcmp(&disk,&s->disk,sizeof(disk))&&
           io_all(fd,meta,disk.meta,false)&&io_all(fd,body,disk.body,false)&&
           metadata_ok(meta,disk.meta,disk.body,s->url)&&
           checksum(body,disk.body,checksum(meta,disk.meta,1))==disk.sum;
        if(fd>=0)close(fd);
        if(!ok) { free(meta);free(body);forget(i);return NSERROR_NOT_FOUND; }
        if(kind) {
            s->buffer[1]=meta;
            if(!s->buffer[0]) { s->buffer[0]=body;warm=i; }
            else free(body);
        } else { s->buffer[0]=body;free(meta); }
    }
    if(s->refs[kind]==65535) return NSERROR_NOMEM;
    s->refs[kind]++;
    if(!kind&&warm==i)warm=-1;
    *data=s->buffer[kind];*length=kind?s->disk.meta:s->disk.body;
    return NSERROR_OK;
}
nserror c16_cache_clear(void)
{
    unsigned i;
    if(lock_fd<0)return NSERROR_NOT_FOUND;
    for(i=0;i<SLOTS;i++)forget(i);
    return NSERROR_OK;
}
static nserror finalise(void)
{
    unsigned i;
    for(i=0;i<SLOTS;i++) {
        free(slots[i].url);free(slots[i].buffer[0]);free(slots[i].buffer[1]);
    }
    memset(slots,0,sizeof(slots));
    if(lock_fd>=0)close(lock_fd);
    lock_fd=warm=-1;return NSERROR_OK;
}
struct gui_llcache_table c16_cache_table={
    .initialise=initialise,.finalise=finalise,.store=store,.fetch=fetch,.release=release,.invalidate=invalidate,
};
