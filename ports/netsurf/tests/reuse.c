/* Real NetSurf fetch lifecycle against persistent HTTP and TLS connections. */
#define main core_acceptance_main
#include "core.c"
#undef main
#include "../http/fetch.h"
#include <sys/wait.h>
static bool fetched,failed;
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
    }
    if(message->type==FETCH_FINISHED||message->type==FETCH_NOTMODIFIED)fetched=true;
    if(message->type==FETCH_REDIRECT) { redirects++;fetched=true; }
    if(message->type==FETCH_ERROR||message->type==FETCH_TIMEDOUT||message->type==FETCH_SSL_ERR||message->type==FETCH_CERT_ERR) {
        failed=fetched=true;snprintf(network_error,sizeof(network_error),"%s",message->data.error);
    }
}
static bool spin(void)
{
    uint64_t now,start;
    nsu_getmonotonic_ms(&start);
    do {
        poll_callbacks();nsu_getmonotonic_ms(&now);
        if(fetched)return true;
        usleep(2000);
    } while(now-start<300000);
    return false;
}

int main(int argc,char **argv)
{
    const char *paths[]={"one","two","chunked","redirect","after","stale","retry","bad","after-bad"};
    const char *bodies[]={"ONE","TWO","ABCDEF","","AFTER","STALE","RETRY",NULL,"RECOVERED"};
    const char *cookie[]={"Cookie: first=yes",NULL};
    unsigned secure,i;char address[100],expected[40];int status;
    check(netsurf_register(&tables)==NSERROR_OK,"frontend registration");
    check(nslog_init(NULL,&argc,argv)==NSERROR_OK,"logging init");
    check(nsoption_init(NULL,NULL,NULL)==NSERROR_OK,"options init");
    check(netsurf_init(NULL)==NSERROR_OK,"browser core init");
    check(netsurf_http_register(NULL)==NSERROR_OK,"HTTP HTTPS fetcher registration");
    for(secure=0;secure<2;secure++)for(i=0;i<9;i++) {
        nsurl *url;
        snprintf(address,sizeof(address),"%s://tls.test/%s",secure?"https":"http",paths[i]);
        fetched=failed=false;bytes=headers=redirects=0;network_error[0]=first_data[0]=0;
        check(nsurl_create(address,&url)==NSERROR_OK,"URL");
        check(fetch_start(url,NULL,receive,NULL,false,NULL,NULL,true,false,i==0?cookie:NULL,&fetching)==NSERROR_OK,"fetch");
        nsurl_unref(url);check(spin(),"fetch completed");
        if(!bodies[i])check(failed&&network_error[0],"malformed framing fails");
        else {
            check(!failed,"successful request");
            if(i==3)check(redirects==1&&!bytes,"redirect leaves body unread");
            else {
                snprintf(expected,sizeof(expected),"%s%s",secure?"TLS-":"",secure&&i==2?"CHUNKED":bodies[i]);
                check(!strcmp(expected,first_data)&&bytes==strlen(expected),"exact entity from correct response");
                check(waitpid(-1,&status,WNOHANG)==0,"worker stays alive after fetch");
            }
        }
        fprintf(stderr,"REUSE_REQUEST %s bytes=%lu error=%s\n",address,bytes,network_error);
    }
    netsurf_exit();
    check(waitpid(-1,&status,WNOHANG)<0&&errno==ECHILD,"browser shutdown reaps all workers");
    nsoption_finalise(NULL,NULL);nslog_finalise();
    printf("NETSURF_REUSE checks=%d failures=%d\n",checks,failures);return failures?1:0;
}
