/* Live document references must share no-store images without HTTP caching. */
#define main core_acceptance_main
#include "core.c"
#undef main
#include "../http/fetch.h"
#include "../image/content.h"
#include "content/hlcache.h"
struct observed { bool done, error; unsigned loading, ready; };
static nserror shared_event(hlcache_handle *h,const hlcache_event *event,void *pw)
{
    struct observed *o=pw;
    if(event->type==CONTENT_MSG_LOADING)o->loading++;
    if(event->type==CONTENT_MSG_READY)o->ready++;
    if(event->type==CONTENT_MSG_DONE)o->done=true;
    if(event->type==CONTENT_MSG_ERROR)o->error=o->done=true;
    return NSERROR_OK;
}
static void wait_image(struct observed *o)
{
    uint64_t start,now;nsu_getmonotonic_ms(&start);
    do {poll_callbacks();usleep(2000);nsu_getmonotonic_ms(&now);}while(!o->done&&now-start<20000);
    check(o->done&&!o->error&&o->loading==1&&o->ready==1,"exact loading/ready/done replay");
}
static void wait_page(struct browser_window *bw)
{
    uint64_t start,now;bool done=false;nsu_getmonotonic_ms(&start);
    do {
        poll_callbacks();
        done=browser_window_has_content(bw)&&!browser_window_stop_available(bw);
        usleep(2000);nsu_getmonotonic_ms(&now);
    }while(!done&&now-start<90000);
    check(done,"HTML document and all images loaded");
    check(!strcmp(browser_window_get_title(bw),"Fetch benchmark"),"correct document title");
}
int main(int argc,char **argv)
{
    hlcache_handle *a=NULL,*b=NULL,*c=NULL,*d=NULL,*cancelled=NULL;
    hlcache_child_context child={"UTF-8",false};
    struct browser_window *bw=NULL;nsurl *next=NULL;
    struct observed seen[5]={{0}};nsurl *url=NULL,*parent=NULL,*other=NULL;
    check(netsurf_register(&tables)==NSERROR_OK,"frontend");
    check(nslog_init(NULL,&argc,argv)==NSERROR_OK,"logging");
    check(nsoption_init(NULL,NULL,NULL)==NSERROR_OK,"options");
    check(netsurf_init(NULL)==NSERROR_OK,"core");
    check(netsurf_http_register(NULL)==NSERROR_OK,"transport");
    check(netsurf_images_register()==NSERROR_OK,"image handlers");
    check(nsurl_create("http://tls.test/image-0.png",&url)==NSERROR_OK,"image URL");
    check(nsurl_create("http://tls.test/page.html",&parent)==NSERROR_OK,"parent URL");
    check(nsurl_create("http://tls.test/image-1.png",&other)==NSERROR_OK,"other URL");
    check(hlcache_handle_retrieve(url,HLCACHE_RETRIEVE_SNIFF_TYPE,parent,NULL,
        shared_event,&seen[0],&child,CONTENT_IMAGE,&a)==NSERROR_OK,"pending original");
    check(hlcache_handle_retrieve_shared_image(url,parent,shared_event,&seen[1],&child,a,&b)==NSERROR_OK,"share pending image");
    check(hlcache_handle_retrieve_shared_image(url,parent,shared_event,&seen[4],&child,a,&cancelled)==NSERROR_OK,"cancel pending reference");
    check(hlcache_handle_release(cancelled)==NSERROR_OK,"release before replay");
    wait_image(&seen[0]);wait_image(&seen[1]);
    check(hlcache_handle_get_content(a)==hlcache_handle_get_content(b),"pending references decode once");
    hlcache_handle_release(a);
    check(hlcache_handle_retrieve_shared_image(url,parent,shared_event,&seen[2],&child,b,&c)==NSERROR_OK,"share completed image");
    wait_image(&seen[2]);
    check(hlcache_handle_get_content(b)==hlcache_handle_get_content(c),"late reference reuses decoded content");
    hlcache_handle_release(b);
    check(hlcache_handle_retrieve(url,HLCACHE_RETRIEVE_SNIFF_TYPE,parent,NULL,
        shared_event,&seen[3],&child,CONTENT_IMAGE,&d)==NSERROR_OK,"independent retrieval");
    wait_image(&seen[3]);
    check(hlcache_handle_get_content(c)!=hlcache_handle_get_content(d),"no-store is not reused by ordinary retrieval");
    hlcache_handle_release(d);seen[3]=(struct observed){0};
    check(hlcache_handle_retrieve_shared_image(other,parent,shared_event,&seen[3],&child,c,&d)==NSERROR_OK,"different URL falls back to fetch");
    wait_image(&seen[3]);
    check(hlcache_handle_get_content(c)!=hlcache_handle_get_content(d),"different URL cannot alias image");
    check(!seen[4].done&&!seen[4].loading,"cancelled reference receives no callbacks");
    hlcache_handle_release(c);hlcache_handle_release(d);
    nsoption_set_bool(foreground_images,true);nsoption_set_bool(background_images,true);
    check(browser_window_create(BW_CREATE_HISTORY,parent,NULL,NULL,&bw)==NSERROR_OK,"HTML image loading");
    if(bw) {
        wait_page(bw);
        check(nsurl_create("http://tls.test/next.html",&next)==NSERROR_OK,"next document URL");
        check(browser_window_navigate(bw,next,NULL,BW_NAVIGATE_HISTORY,NULL,NULL,NULL)==NSERROR_OK,"new document with same image URLs");
        wait_page(bw);browser_window_destroy(bw);nsurl_unref(next);
    }
    nsurl_unref(url);nsurl_unref(parent);nsurl_unref(other);
    netsurf_exit();nsoption_finalise(NULL,NULL);nslog_finalise();
    printf("NETSURF_SHARED checks=%d failures=%d\n",checks,failures);return failures?1:0;
}
