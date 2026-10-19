/* SPDX-License-Identifier: GPL-2.0-only */
#define main core_acceptance_main
#include "core.c"
#undef main
#include "../http/fetch.h"
#include <c16-cache.h>
#include <sys/stat.h>
#include <sys/wait.h>
static bool finished,fetch_failed;
static nserror event(llcache_handle *handle,const llcache_event *e,void *pw)
{
    if(e->type==LLCACHE_EVENT_DONE)finished=true;
    if(e->type==LLCACHE_EVENT_ERROR) {
        fprintf(stderr,"CACHE_FETCH_ERROR %s\n",e->data.error.msg?e->data.error.msg:"fetch failed");
        finished=fetch_failed=true;
    }
    return NSERROR_OK;
}
static bool retrieve(const char *address,uint32_t flags)
{
    nsurl *url=NULL;llcache_handle *handle=NULL;uint64_t start,now;size_t length;
    const ns_octet_t *body;bool ok=false;
    if(nsurl_create(address,&url)!=NSERROR_OK)return false;
    finished=fetch_failed=false;
    if(llcache_handle_retrieve(url,flags,NULL,NULL,event,NULL,&handle)!=NSERROR_OK)goto done;
    nsu_getmonotonic_ms(&start);
    do { poll_callbacks();usleep(2000);nsu_getmonotonic_ms(&now); }while(!finished&&now-start<180000);
    body=llcache_handle_get_source_data(handle,&length);
    ok=finished&&!fetch_failed&&body!=NULL;
    if(strstr(address,"/large"))ok=ok&&length==70000UL&&body[0]=='Z'&&body[69999UL]=='Z';
    else ok=ok&&length==6&&!memcmp(body,"cached",6);
    if(!ok)fprintf(stderr,"CACHE_FETCH_RESULT done=%u error=%u cells=%lu elapsed_ms=%lu\n",
        finished?1:0,fetch_failed?1:0,(unsigned long)length,(unsigned long)(now-start));
    llcache_handle_release(handle);
done:
    nsurl_unref(url);return ok;
}
static nserror put(nsurl *url,unsigned long length)
{
    ns_octet_t *body=malloc(length?length:1);char *meta=malloc(512);int n; nserror error;
    if(!body||!meta) { free(body);free(meta);return NSERROR_NOMEM; }
    memset(body,'Z',length);
    n=snprintf(meta,512,"%s%c%lu%c1770000000%c1770000000%c1770000000%c0%c0%c",
        nsurl_access(url),0,length,0,0,0,0,0,0);
    error=c16_cache_table.store(url,BACKING_STORE_NONE,body,length);
    if(error!=NSERROR_OK) { free(body);free(meta);return error; }
    error=c16_cache_table.store(url,BACKING_STORE_META,(ns_octet_t *)meta,n);
    if(error==NSERROR_OK)c16_cache_table.release(url,BACKING_STORE_META);
    else { free(meta);c16_cache_table.invalidate(url); }
    c16_cache_table.release(url,BACKING_STORE_NONE);
    return error;
}
static void store_tests(void)
{
    struct llcache_store_parameters params={.path="/disk-store-test",.limit=100000UL};
    nsurl *a=NULL,*b=NULL;ns_octet_t *data=NULL;size_t n;int fd,i;
    check(nsurl_create("http://tls.test/a",&a)==NSERROR_OK,"store URL A");
    check(nsurl_create("http://tls.test/b",&b)==NSERROR_OK,"store URL B");
    check(c16_cache_table.initialise(&params)==NSERROR_OK&&c16_cache_enabled(),"store init");
    {
        pid_t child=fork();int status=-1;
        if(!child) { char *args[]={"diskcachecheck","--locked",NULL};execv("/bin/diskcachecheck",args);_exit(126); }
        check(child>0&&waitpid(child,&status,0)==child&&WIFEXITED(status)&&WEXITSTATUS(status)==0,
              "second process falls back while directory is locked");
    }
    c16_cache_clear();
    check(put(a,70000UL)==NSERROR_OK,"write across bank and 16-bit length");
    check(c16_cache_table.fetch(a,BACKING_STORE_NONE,&data,&n)==NSERROR_OK&&n==70000UL&&data[69999UL]=='Z',"read across bank");
    check(put(b,70000UL)!=NSERROR_OK,"quota cannot evict pinned body");
    check(data[69999UL]=='Z',"failed store preserves active body");
    c16_cache_table.release(a,BACKING_STORE_NONE);
    check(put(b,70000UL)==NSERROR_OK,"quota evicts unpinned entry");
    check(c16_cache_table.fetch(a,BACKING_STORE_META,&data,&n)==NSERROR_NOT_FOUND,"quota victim misses");
    c16_cache_table.finalise();
    check(c16_cache_table.initialise(&params)==NSERROR_OK,"reopen store");
    check(c16_cache_table.fetch(b,BACKING_STORE_NONE,&data,&n)==NSERROR_OK&&n==70000UL&&data[69999UL]=='Z',"reopen preserves source");
    check(c16_cache_clear()==NSERROR_OK&&data[69999UL]=='Z',"clear keeps active borrowed buffer");
    c16_cache_table.release(b,BACKING_STORE_NONE);
    check(c16_cache_table.fetch(b,BACKING_STORE_META,&data,&n)==NSERROR_NOT_FOUND,"clear removes stored object");
    check(put(a,10)==NSERROR_OK,"corrupt fixture write");
    fd=open("/disk-store-test/000.cache",O_WRONLY);
    check(fd>=0&&lseek(fd,-1,SEEK_END)>=0&&write(fd,"Q",1)==1,"tamper body");if(fd>=0)close(fd);
    check(c16_cache_table.fetch(a,BACKING_STORE_META,&data,&n)==NSERROR_NOT_FOUND,"checksum rejects corrupt body before metadata");
    check(put(a,10)==NSERROR_OK,"truncated fixture write");
    fd=open("/disk-store-test/000.cache",O_WRONLY|O_TRUNC);if(fd>=0)close(fd);
    c16_cache_table.finalise();c16_cache_table.initialise(&params);
    check(c16_cache_table.fetch(a,BACKING_STORE_META,&data,&n)==NSERROR_NOT_FOUND,"restart rejects truncated record");
    mkdir("/disk-store-test/000.tmp",0700);
    check(put(a,10)!=NSERROR_OK,"write failure is recoverable");
    rmdir("/disk-store-test/000.tmp");
    check(put(a,10)==NSERROR_OK,"store recovers after write failure");
    c16_cache_clear();
    for(i=0;i<132;i++) {
        char address[96];nsurl *url;
        snprintf(address,sizeof(address),"http://tls.test/entry/%d",i);
        if(nsurl_create(address,&url)!=NSERROR_OK) { check(false,"eviction URL");break; }
        if(put(url,10)!=NSERROR_OK)check(false,"slot eviction write");
        nsurl_unref(url);
    }
    check(i==132,"more than 128 entries evict cleanly");
    c16_cache_clear();c16_cache_table.finalise();
    params.path="/missing-parent/cache";
    check(c16_cache_table.initialise(&params)==NSERROR_OK&&!c16_cache_enabled(),"unavailable directory falls back to memory");
    c16_cache_table.finalise();nsurl_unref(a);nsurl_unref(b);
}
int main(int argc,char **argv)
{
    int i;bool force=false,expect_error=false;bool storage=argc>1&&!strcmp(argv[1],"--store");
    bool locked=argc>1&&!strcmp(argv[1],"--locked");
    tables.llcache=storage?NULL:&c16_cache_table;
    check(netsurf_register(&tables)==NSERROR_OK,"register");
    check(nslog_init(NULL,&argc,argv)==NSERROR_OK,"logging");
    check(nsoption_init(NULL,NULL,NULL)==NSERROR_OK,"options");
    check(netsurf_init(storage?NULL:locked?"/disk-store-test":"/.netsurf-cache")==NSERROR_OK,"core");
    if(storage)store_tests();
    else {
        check(c16_cache_enabled()!=locked,"disk cache enabled or locked fallback");
        check(netsurf_http_register(NULL)==NSERROR_OK,"transport");
        if(argc>1&&!strcmp(argv[1],"--clear"))check(c16_cache_clear()==NSERROR_OK,"clear");
        for(i=1;i<argc;i++) {
            if(!strcmp(argv[i],"--force"))force=true;
            else if(!strcmp(argv[i],"--error"))expect_error=true;
            else if(argv[i][0]!='-')check(retrieve(argv[i],force?LLCACHE_RETRIEVE_FORCE_FETCH:0)!=expect_error,argv[i]);
        }
    }
    netsurf_exit();nsoption_finalise(NULL,NULL);nslog_finalise();
    printf("NETSURF_DISK checks=%d failures=%d\n",checks,failures);return failures?1:0;
}
