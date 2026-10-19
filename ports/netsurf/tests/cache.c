/* SPDX-License-Identifier: GPL-2.0-only */
#define main core_acceptance_main
#include "core.c"
#undef main
#include "../http/fetch.h"
#include "content/llcache.h"
static bool finished,fetch_failed;
static nserror cache_event(llcache_handle *handle,const llcache_event *event,void *pw)
{
    if(event->type==LLCACHE_EVENT_DONE)finished=true;
    if(event->type==LLCACHE_EVENT_ERROR)finished=fetch_failed=true;
    return NSERROR_OK;
}
int main(int argc,char **argv)
{
    const char *paths[]={"fresh","validate","private"};unsigned i,j;
    check(netsurf_register(&tables)==NSERROR_OK,"register");
    check(nslog_init(NULL,&argc,argv)==NSERROR_OK,"logging");
    check(nsoption_init(NULL,NULL,NULL)==NSERROR_OK,"options");
    check(netsurf_init(NULL)==NSERROR_OK,"core");
    check(netsurf_http_register(NULL)==NSERROR_OK,"transport");
    for(i=0;i<3;i++)for(j=0;j<2;j++) {
        nsurl *url=NULL;llcache_handle *handle=NULL;char address[100];uint64_t start,now;
        size_t length;const ns_octet_t *body;
        snprintf(address,sizeof(address),"http://tls.test/cache/%s",paths[i]);
        check(nsurl_create(address,&url)==NSERROR_OK,"URL");
        finished=fetch_failed=false;
        check(llcache_handle_retrieve(url,0,NULL,NULL,cache_event,NULL,&handle)==NSERROR_OK,"retrieve");
        nsu_getmonotonic_ms(&start);
        do { poll_callbacks();usleep(2000);nsu_getmonotonic_ms(&now); }while(!finished&&now-start<20000);
        check(finished&&!fetch_failed,"response");
        body=llcache_handle_get_source_data(handle,&length);
        check(length==6&&!memcmp(body,"cached",6),"same body including 304 reuse");
        llcache_handle_release(handle);nsurl_unref(url);
    }
    netsurf_exit();nsoption_finalise(NULL,NULL);nslog_finalise();
    printf("NETSURF_CACHE checks=%d failures=%d\n",checks,failures);return failures?1:0;
}
