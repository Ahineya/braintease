/* Slow resources must not serialize unrelated fetches or delay size refusal. */
#define main single_request_main
#include "core.c"
#undef main
#include "../http/fetch.h"
#include "content/fetch.h"
struct transfer_check { struct fetch *handle;unsigned done,error;unsigned long bytes;char reason[160]; };
static void transfer_event(const fetch_msg *message,void *opaque) {
    struct transfer_check *t=opaque;
    if(message->type==FETCH_DATA)t->bytes+=message->data.header_or_data.len;
    if(message->type==FETCH_FINISHED)t->done=1;
    if(message->type==FETCH_ERROR||message->type==FETCH_TIMEDOUT) {
        t->done=t->error=1;snprintf(t->reason,sizeof(t->reason),"%s",message->data.error);
    }
}
int main(int argc,char **argv) {
    const char *paths[]={"held","fast","huge","third"};
    struct transfer_check t[4];nsurl *url;unsigned i;char address[80];
    uint64_t start,now;unsigned long limit=argc>1?strtoul(argv[1],NULL,10)*1000UL:20000UL;
    memset(t,0,sizeof(t));
    check(netsurf_register(&tables)==NSERROR_OK,"register frontend");
    check(nslog_init(NULL,&argc,argv)==NSERROR_OK,"logging");
    check(nsoption_init(NULL,NULL,NULL)==NSERROR_OK,"options");
    check(netsurf_init(NULL)==NSERROR_OK,"core init");
    check(netsurf_http_register(NULL)==NSERROR_OK,"HTTP init");
    if(failures)return 1;
    for(i=0;i<4;i++) {
        snprintf(address,sizeof(address),"http://tls.test/%s",paths[i]);
        check(nsurl_create(address,&url)==NSERROR_OK,"URL");
        check(fetch_start(url,NULL,transfer_event,t+i,false,NULL,NULL,true,false,NULL,&t[i].handle)==NSERROR_OK,"fetch");
        nsurl_unref(url);
    }
    nsu_getmonotonic_ms(&start);
    do {
        poll_callbacks();nsu_getmonotonic_ms(&now);
        if(t[0].done)break;
        if(t[1].done&&t[2].done&&t[3].done)break;
        usleep(2000);
    } while(now-start<limit);
    check(t[0].bytes==1&&!t[0].done,"slow body stays in progress while others complete");
    check(t[1].done&&!t[1].error&&t[1].bytes==4,"fast request passes blocked resource");
    check(t[2].done&&t[2].error&&!t[2].bytes&&strstr(t[2].reason,"source limit"),"oversize rejected at headers, before body");
    check(t[3].done&&!t[3].error&&t[3].bytes==5,"queued request starts when a worker is released");
    if(!t[0].done)fetch_abort(t[0].handle);
    for(i=1;i<4;i++)if(!t[i].done)fetch_abort(t[i].handle);
    for(i=0;i<100;i++){poll_callbacks();usleep(2000);}
    netsurf_exit();nsoption_finalise(NULL,NULL);nslog_finalise();
    printf("NETSURF_STREAMING checks=%d failures=%d elapsed_ms=%lu\n",checks,failures,(unsigned long)(now-start));
    return failures?1:0;
}
