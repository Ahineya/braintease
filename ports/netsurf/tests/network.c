/* SPDX-License-Identifier: GPL-2.0-only */
/* Exercise actual browser layout plus the independent guest network worker. */
#define main headless_acceptance_main
#include "core.c"
#undef main
#include "../http/fetch.h"
#include <sys/select.h>
static bool fetched,failed,abort_on_data;
static unsigned long bytes;
static unsigned headers,redirects;
static char network_error[256],first_data[512];
static struct fetch *fetching;
static void receive(const fetch_msg *message,void *context)
{
    if(message->type==FETCH_HEADER)headers++;
    if(message->type==FETCH_DATA) {
        size_t n=message->data.header_or_data.len;
        if(!bytes) { if(n>=sizeof(first_data))n=sizeof(first_data)-1;
            memcpy(first_data,message->data.header_or_data.buf,n);first_data[n]=0; }
        bytes+=message->data.header_or_data.len;
        if(abort_on_data) { fetch_abort(fetching); fetched=true; }
    }
    if(message->type==FETCH_FINISHED||message->type==FETCH_NOTMODIFIED)fetched=true;
    if(message->type==FETCH_REDIRECT) { redirects++;fetched=true; }
    if(message->type==FETCH_ERROR||message->type==FETCH_TIMEDOUT||message->type==FETCH_SSL_ERR||message->type==FETCH_CERT_ERR) {
        failed=fetched=true;snprintf(network_error,sizeof(network_error),"%s",message->data.error);
    }
}
static bool spin(bool browser,struct browser_window *bw,const char *expected_title)
{
    uint64_t now,start,ready=0;
    nsu_getmonotonic_ms(&start);
    for(;;) {
        poll_callbacks();
        nsu_getmonotonic_ms(&now);
        if(!browser&&fetched)return true;
        if(browser&&browser_window_has_content(bw)&&!browser_window_stop_available(bw)&&
           (!expected_title||!strcmp(browser_window_get_title(bw),expected_title))) {
            if(!ready)ready=now;
            if(now-ready>=1200)return true; /* Permit immediate HTML meta refresh. */
        } else ready=0;
        if(now-start>(expected_title?15000:300000))return false;
        usleep(2000);
    }
}
int main(int argc,char **argv)
{
    struct browser_window *bw=NULL;
    nsurl *url=NULL;
    const char *ca=NULL,*address=NULL,*expected=NULL,*recovery=NULL;
    bool raw=false,expect_error=false;
    int i;
    for(i=1;i<argc;i++) {
        if(!strcmp(argv[i],"--cacert")&&i+1<argc)ca=argv[++i];
        else if(!strcmp(argv[i],"--recover")&&i+1<argc)recovery=argv[++i];
        else if(!strcmp(argv[i],"--raw"))raw=true;
        else if(!strcmp(argv[i],"--error")) {raw=true;expect_error=true;}
        else if(!strcmp(argv[i],"--abort")) {raw=true;abort_on_data=true;}
        else if(!address)address=argv[i]; else expected=argv[i];
    }
    if(!address){fputs("networkcheck [--cacert FILE] [--raw|--error|--abort] [--recover URL] URL [EXPECTED]",stderr);return 2;}
    check(netsurf_register(&tables)==NSERROR_OK,"frontend registration");
    check(nslog_init(NULL,&argc,argv)==NSERROR_OK,"logging init");
    check(nsoption_init(NULL,NULL,NULL)==NSERROR_OK,"options init");
    nsoption_set_bool(foreground_images,false);nsoption_set_bool(background_images,false);
    nsoption_set_bool(incremental_reflow,false);
    check(netsurf_init(NULL)==NSERROR_OK,"browser core init");
    check(netsurf_http_register(ca)==NSERROR_OK,"HTTP HTTPS fetcher registration");
    check(nsurl_create(address,&url)==NSERROR_OK,"network URL");
    if(failures)return 1;
    if(raw) {
        check(fetch_start(url,NULL,receive,NULL,false,NULL,NULL,true,false,NULL,&fetching)==NSERROR_OK,"network fetch start");
        check(spin(false,NULL,NULL),"network fetch completion");
        if(expect_error)check(failed&&network_error[0],"failed request reports error");
        else if(abort_on_data)check(bytes>0,"abort during body callback");
        else check(!failed&&headers>0&&(bytes>0||redirects>0),"HTTP entity delivered");
        if(expected)check(strstr(first_data,expected)!=NULL||strstr(network_error,expected)!=NULL,"expected transport output");
        fprintf(stderr,"NETWORK_RESULT bytes=%lu headers=%u redirects=%u error=%s\n",bytes,headers,redirects,network_error);
        /* Flush abort/child reaping before shutting down the fetcher factory. */
        for(i=0;i<50;i++){poll_callbacks();usleep(2000);}
    } else {
        struct rect clip={0,0,480,1000};
        struct redraw_context redraw={.interactive=false,.background_images=true,.plot=&plotters};
        check(browser_window_create(BW_CREATE_HISTORY,url,NULL,NULL,&bw)==NSERROR_OK,"network browser create");
        check(spin(true,bw,NULL),"network page and styles complete");
        drawn[0]=0;drawn_length=0;
        check(browser_window_redraw(bw,0,0,&clip,&redraw),"network HTML redraw");
        fprintf(stderr,"NETWORK_PAGE title=%s url=%s text=%s\n",browser_window_get_title(bw),nsurl_access(browser_window_access_url(bw)),drawn);
        if(expected)check(strstr(drawn,expected)!=NULL||strstr(browser_window_get_title(bw),expected)!=NULL,"expected rendered page");
        if(recovery)for(i=0;i<6;i++) {
            nsurl *next=NULL;
            char failed_url[1400];
            bool created;
            /* Wiby's random links fail at different destinations. Exercise
             * fresh failed fetches rather than cached results of one URL. */
            snprintf(failed_url,sizeof(failed_url),"%s%crecovery=%d",address,strchr(address,'?')?'&':'?',i);
            created=nsurl_create(i%2?failed_url:recovery,&next)==NSERROR_OK;
            check(created,"recovery URL");
            if(!created)break;
            check(browser_window_navigate(bw,next,NULL,BW_NAVIGATE_HISTORY,NULL,NULL,NULL)==NSERROR_OK,"navigate after error page");
            nsurl_unref(next);
            check(spin(true,bw,i%2?"FetchErrorTitle":"Network CSS page"),"recovery page settled");
            drawn[0]=0;drawn_length=0;
            check(browser_window_redraw(bw,0,0,&clip,&redraw),"recovery page redraw");
            check(strstr(drawn,i%2?"FetchErrorTitle":"HTTP and external CSS")!=NULL,"error page and successful page remain usable");
            fprintf(stderr,"NETWORK_RECOVERED %d title=%s text=%s\n",i,browser_window_get_title(bw),drawn);
        }
        browser_window_destroy(bw);
    }
    nsurl_unref(url);netsurf_exit();nsoption_finalise(NULL,NULL);nslog_finalise();
    printf("NETSURF_NETWORK checks=%d failures=%d\n",checks,failures);
    return failures?1:0;
}
