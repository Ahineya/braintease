/* SPDX-License-Identifier: GPL-2.0-only */
/* Asynchronous NetSurf adapter to the generic guest htget --worker protocol.
 * Standard pipes carry HTTP entity headers/body and diagnostics. TLS/crypto
 * objects live entirely in the exec'd worker, never in the browser ABI. */
#include <netsurf_profile.h>
#include "image-limits.h"
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <errno.h>
#include <signal.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <time.h>
#include <libwapcaplet/libwapcaplet.h>
#include "utils/nsurl.h"
#include "content/fetch.h"
#include "content/fetchers.h"
#include "content/urldb.h"
#include "fetch.h"
#include "../../tls/ripple_http.h"
#define WEB_HEADERS 4
#define WEB_LINE 1024
#define WEB_BODY_LIMIT web_body_limit
static unsigned long web_body_limit;
#define WEB_WORKERS 4
static unsigned web_worker_limit=2;
struct web_worker {
    pid_t pid;
    int input,output,errors,status;
    bool busy,exited;
    char origin[1280];
};
static struct web_worker workers[WEB_WORKERS];
struct web_fetch {
    struct web_fetch *next;
    struct fetch *parent;
    nsurl *url;
    struct web_worker *worker;
    char *request;
    unsigned request_size,request_sent,record_used,record_left,record_kind;
    unsigned char record[3];
    bool draining,started,aborted,locked,eof,headers_done,progress,only_2xx,known;
    unsigned line_used,header_budget,header_count,error_used;
    unsigned long received,total,started_at,drain_at;
    const char *failure;
    char line[WEB_LINE+3], error[256], *location, *headers[WEB_HEADERS];
};
static struct web_fetch *jobs;
static unsigned active_count;
static char *ca_path;
void netsurf_http_transfer(struct netsurf_transfer *t)
{
    memset(t,0,sizeof(*t));
    struct web_fetch *c;
    for(c=jobs;c;c=c->next)if(c->started&&!c->aborted&&!c->failure&&!c->eof) {
        t->active=true;t->received=c->received;t->total=c->total;
        t->known=c->known;t->url=nsurl_access(c->url);
        break;
    }
}
static unsigned long now_ms(void)
{
    struct timespec t;
    if(clock_gettime(CLOCK_MONOTONIC,&t)<0)return 0;
    return (unsigned long)t.tv_sec*1000UL+(unsigned long)t.tv_nsec/1000000UL;
}
static void close_pipe(int *fd) { if(*fd>=0)close(*fd); *fd=-1; }
static void stop_worker(struct web_worker *w)
{
    if(!w||w->pid<=0)return;
    close_pipe(&w->input);close_pipe(&w->output);close_pipe(&w->errors);
    if(!w->exited)kill(w->pid,SIGKILL);
}
static void kill_worker(struct web_fetch *c) { stop_worker(c->worker); }
static bool reap_worker(struct web_worker *w,bool block)
{
    pid_t result;
    if(!w||w->pid<=0||w->exited)return true;
    do { result=waitpid(w->pid,&w->status,block?0:WNOHANG); } while(result<0&&errno==EINTR&&block);
    if(result==w->pid||(result<0&&errno==ECHILD)) { w->exited=true;return true; }
    return false;
}
static bool collect(struct web_fetch *c,bool block) { return reap_worker(c->worker,block); }
static void release_worker(struct web_fetch *c)
{
    if(c->worker){c->worker->busy=false;c->worker=NULL;}
}
static void destroy(void *opaque)
{
    struct web_fetch *c=opaque,**at=&jobs;
    unsigned i;
    while(*at&&*at!=c)at=&(*at)->next;
    if(*at)*at=c->next;
    if(getenv("NETSURF_TRACE_HTTP"))fprintf(stderr,"NETSURF_HTTP_FREE %s started=%d aborted=%d received=%lu\n",nsurl_access(c->url),c->started,c->aborted,c->received);
    if(c->started)--active_count;
    kill_worker(c); collect(c,true); release_worker(c);free(c->request);
    nsurl_unref(c->url); free(c->location);
    for(i=0;i<c->header_count;i++)free(c->headers[i]);
    free(c);
}
static void send_message(struct web_fetch *c,const fetch_msg *message)
{
    c->locked=true; fetch_send_callback(message,c->parent); c->locked=false;
}
static void fail(struct web_fetch *c,const char *reason)
{
    if(!c->failure)c->failure=reason;
    if(getenv("NETSURF_TRACE_HTTP"))fprintf(stderr,"NETSURF_HTTP_ERROR %s %s\n",nsurl_access(c->url),reason);
    kill_worker(c); c->eof=true;
}
static bool initialise(lwc_string *scheme) { return true; }
static bool acceptable(const nsurl *url)
{
    const char *s=nsurl_access(url);
    return nsurl_length(url)<1280&&(!strncmp(s,"http://",7)||!strncmp(s,"https://",8));
}
static void *setup(struct fetch *parent,nsurl *url,bool only_2xx,bool downgrade,
                   const char *post,const struct fetch_multipart_data *multipart,const char **headers)
{
    struct web_fetch *c=calloc(1,sizeof(*c));
    unsigned i;
    if(!c)return NULL;
    c->parent=parent; c->url=nsurl_ref(url); c->only_2xx=only_2xx;
    c->header_budget=16384;
    c->next=jobs; jobs=c;
    if(!acceptable(url))c->failure="HTTP URL exceeds this browser's bounds";
    else if(post||multipart)c->failure="POST and uploads are not enabled; GET forms are supported";
    else if(downgrade)c->failure="TLS protocol downgrade is disabled";
    for(i=0;headers&&headers[i];i++) {
        if(c->header_count==WEB_HEADERS||strlen(headers[i])>WEB_LINE||strchr(headers[i],'\r')||strchr(headers[i],'\n')) {
            c->failure="Request headers exceed this browser's bounds"; break;
        }
        c->headers[c->header_count]=strdup(headers[i]);
        if(!c->headers[c->header_count]) { destroy(c); return NULL; }
        c->header_count++;
    }
    return c;
}
/* Prefer an idle connection to this origin; workers retain only one origin.
 * Failed/cancelled jobs discard their worker, including all buffered bytes. */
static struct web_worker *available_worker(const char *url)
{
    struct web_worker *choice=NULL,*w;unsigned i,n;
    const char *at=strstr(url,"://");
    if(!at)return NULL;
    at+=3;while(*at&&*at!='/'&&*at!='?'&&*at!='#')at++;
    n=at-url;
    for(i=0;i<web_worker_limit;i++) {
        w=&workers[i];if(w->busy)continue;
        if(w->pid>0&&reap_worker(w,false)) {stop_worker(w);w->pid=0;w->origin[0]=0;}
        if(!choice||(!choice->pid&&w->pid>0))choice=w;
        if(strlen(w->origin)==n&&!strncasecmp(w->origin,url,n)){choice=w;break;}
    }
    if(choice){memcpy(choice->origin,url,n);choice->origin[n]=0;}
    return choice;
}
static bool spawn_worker(struct web_worker *w)
{
    int input[2]={-1,-1},out[2]={-1,-1},err[2]={-1,-1},fd;
    if(pipe(input)<0||pipe(out)<0||pipe(err)<0)goto failed;
    w->pid=fork();w->exited=false;
    if(w->pid<0)goto failed;
    if(!w->pid) {
        char *args[7],limit[12];unsigned count=0;
        if(dup2(input[0],0)<0||dup2(out[1],1)<0||dup2(err[1],2)<0)_exit(126);
        /* Never keep the browser's Nano-X/socket/files alive after exec. */
        for(fd=3;fd<FD_SETSIZE;fd++)close(fd);
        args[count++]="htget";args[count++]="--worker";
        snprintf(limit,sizeof(limit),"%lu",WEB_BODY_LIMIT);
        args[count++]="--body-limit";args[count++]=limit;
        if(ca_path){args[count++]="--cacert";args[count++]=ca_path;}
        args[count]=NULL;execv("/bin/htget",args);
        write(2,"Unable to execute /bin/htget\n",sizeof("Unable to execute /bin/htget\n")-1);_exit(127);
    }
    close(input[0]);close(out[1]);close(err[1]);
    w->input=input[1];w->output=out[0];w->errors=err[0];
    if(fcntl(w->input,F_SETFL,O_NONBLOCK)<0||fcntl(w->output,F_SETFL,O_NONBLOCK)<0||
       fcntl(w->errors,F_SETFL,O_NONBLOCK)<0){stop_worker(w);return false;}
    return true;
failed:
    for(fd=0;fd<2;fd++){if(input[fd]>=0)close(input[fd]);if(out[fd]>=0)close(out[fd]);if(err[fd]>=0)close(err[fd]);}
    w->pid=0;return false;
}
static bool start(void *opaque)
{
    struct web_fetch *c=opaque;
    char *cookie,*at;unsigned i,length;
    struct web_worker *w;
    if(active_count==web_worker_limit)return false;
    if(!WEB_BODY_LIMIT)c->failure="Invalid NETSURF_IMAGE_INPUT limit";
    if(c->aborted||c->failure){++active_count;c->started=true;c->eof=true;return true;}
    w=available_worker(nsurl_access(c->url));if(!w)return false;
    ++active_count;c->started=true;c->started_at=now_ms();
    cookie=urldb_get_cookie(c->url,true);
    if(cookie) {
        size_t n=strlen(cookie);
        if(c->header_count==WEB_HEADERS||n>WEB_LINE-8)fail(c,"Cookie exceeds this browser's request bounds");
        else {
            char *s=malloc(n+9);
            if(s){strcpy(s,"Cookie: ");strcat(s,cookie);c->headers[c->header_count++]=s;}
            else fail(c,"Unable to allocate cookie header");
        }
        free(cookie);
    }
    if(c->failure)return true;
    length=strlen(nsurl_access(c->url))+2;
    for(i=0;i<c->header_count;i++)length+=strlen(c->headers[i])+1;
    c->request=malloc(length+2);
    if(!c->request){fail(c,"Unable to allocate HTTP worker request");return true;}
    c->request[0]=length>>8;c->request[1]=length&255;
    at=c->request+2;strcpy(at,nsurl_access(c->url));at+=strlen(at);*at++='\n';
    for(i=0;i<c->header_count;i++){strcpy(at,c->headers[i]);at+=strlen(at);*at++='\n';}
    *at='\n';c->request_size=length+2;
    c->worker=w;w->busy=true;
    if(!w->pid&&!spawn_worker(w))fail(c,"Unable to start guest HTTP worker");
    return true;
}
static void abort_fetch(void *opaque)
{
    struct web_fetch *c=opaque;
    if(getenv("NETSURF_TRACE_HTTP"))fprintf(stderr,"NETSURF_HTTP_ABORT %s\n",nsurl_access(c->url));
    c->aborted=true;
    /* Small, already framed responses can be discarded to a validated DONE.
     * This preserves the worker/TLS connection without delivering more callbacks.
     * Unknown/large responses and slow peers are cancelled by closing instead. */
    if(c->worker && c->headers_done && c->known && !c->failure &&
       c->request_sent==c->request_size && c->received<=c->total &&
       c->total-c->received<=65536UL) {
        c->draining=true;c->drain_at=now_ms();
    } else kill_worker(c);
}
static void complete(struct web_fetch *c,fetch_msg_type type,const char *value)
{
    fetch_msg message;
    if(!c->aborted) {
        memset(&message,0,sizeof(message));message.type=type;
        if(type==FETCH_REDIRECT)message.data.redirect=value;
        else if(type==FETCH_ERROR||type==FETCH_TIMEDOUT)message.data.error=value;
        send_message(c,&message);
    }
    fetch_remove_from_queues(c->parent);fetch_free(c->parent);
}
static void process_line(struct web_fetch *c)
{
    fetch_msg message;
    char *colon,*value;
    unsigned n=c->line_used;
    if(n&&c->line[n-1]=='\r')n--;
    c->line[n]=0;
    if(n>WEB_LINE){fail(c,"HTTP worker header line exceeds bounds");return;}
    if(!fetch_http_code(c->parent)) {
        unsigned code;
        if(n<12||(strncmp(c->line,"HTTP/1.0 ",9)&&strncmp(c->line,"HTTP/1.1 ",9))||
           c->line[9]<'1'||c->line[9]>'5'||c->line[10]<'0'||c->line[10]>'9'||c->line[11]<'0'||c->line[11]>'9') {
            fail(c,"Invalid HTTP worker response");return;
        }
        code=(c->line[9]-'0')*100+(c->line[10]-'0')*10+c->line[11]-'0';
        if(code<200){fail(c,"Unexpected informational response from worker");return;}
        fetch_set_http_code(c->parent,(long)code);
    } else if(!n) { c->headers_done=true;return; }
    else {
        colon=strchr(c->line,':');
        if(!colon){fail(c,"Malformed response header");return;}
        value=colon+1;while(*value==' '||*value=='\t')value++;
        if(colon-c->line==8&&!strncasecmp(c->line,"Location",8)) {
            free(c->location);c->location=strdup(value);if(!c->location)fail(c,"Unable to allocate redirect URL");
        }
        if(colon-c->line==14&&!strncasecmp(c->line,"Content-Length",14)) {
            char *end;unsigned long n;
            long code=fetch_http_code(c->parent);
            errno=0;n=strtoul(value,&end,10);
            if(!errno&&end!=value&&!*end) {
                /* Reject known oversize entities at their headers, before
                 * downloading the first 256 KiB of a multi-megabyte image. */
                if(n>WEB_BODY_LIMIT&&code!=204&&code!=304&&code!=301&&code!=302&&code!=303&&code!=307&&code!=308) {
                    fail(c,"HTTP response exceeds configured source limit");return;
                }
                c->total=n;c->known=true;
            }
        }
        if(colon-c->line==10&&!strncasecmp(c->line,"Set-Cookie",10))fetch_set_cookie(c->parent,value);
    }
    c->line[n++]='\r';c->line[n++]='\n';c->line[n]=0;
    memset(&message,0,sizeof(message));message.type=FETCH_HEADER;
    message.data.header_or_data.buf=(const ns_octet_t *)c->line;message.data.header_or_data.len=n;
    send_message(c,&message);
}
static void drain_errors(struct web_fetch *c)
{
    char buf[256];ssize_t got;unsigned i;
    if(!c->worker||c->worker->errors<0)return;
    got=read(c->worker->errors,buf,sizeof(buf));
    if(got>0)for(i=0;i<(unsigned)got;i++) {
        unsigned ch=(unsigned char)buf[i];
        if(c->error_used+1<sizeof(c->error)&&ch>=32&&ch<=126)c->error[c->error_used++]=ch;
        else if((ch=='\n'||ch=='\r')&&c->error_used&&c->error[c->error_used-1]!=' '&&c->error_used+1<sizeof(c->error))c->error[c->error_used++]=' ';
        c->error[c->error_used]=0;
    }
    else if(got==0)close_pipe(&c->worker->errors);
    else if(errno!=EAGAIN&&errno!=EINTR) { close_pipe(&c->worker->errors); fail(c,"HTTP worker diagnostic pipe failed"); }
}
static void response_data(struct web_fetch *c,const ns_octet_t *buf,unsigned got)
{
    unsigned i=0,n,kind;fetch_msg message;
    while(i<got&&!c->eof&&!c->failure&&(!c->aborted||c->draining)) {
        if(c->record_used<3) {
            if(buf[i]>255){fail(c,"Invalid HTTP worker record octet");break;}
            c->record[c->record_used++]=buf[i++];
            if(c->record_used<3)continue;
            kind=c->record_kind=c->record[0];
            c->record_left=(unsigned)c->record[1]*256+c->record[2];
            if((kind==RIPPLE_HTTP_DONE&&c->record_left)||
               (kind==RIPPLE_HTTP_HEADERS&&(!c->record_left||c->record_left>16384||c->headers_done))||
               (kind==RIPPLE_HTTP_BODY&&(!c->headers_done||!c->record_left||c->record_left>4096))||
               (kind==RIPPLE_HTTP_ERROR&&(!c->record_left||c->record_left>1024))||
               (kind!=RIPPLE_HTTP_DONE&&kind!=RIPPLE_HTTP_HEADERS&&kind!=RIPPLE_HTTP_BODY&&kind!=RIPPLE_HTTP_ERROR)) {
                fail(c,"Invalid HTTP worker record");break;
            }
            if(kind==RIPPLE_HTTP_DONE) {
                if(!c->headers_done||(c->known&&c->received!=c->total&&fetch_http_code(c->parent)!=304&&
                   fetch_http_code(c->parent)!=301&&fetch_http_code(c->parent)!=302&&fetch_http_code(c->parent)!=303&&
                   fetch_http_code(c->parent)!=307&&fetch_http_code(c->parent)!=308)||i!=got) {
                    fail(c,"Incomplete HTTP worker response");break;
                }
                c->eof=true;c->draining=false;release_worker(c);break;
            }
            if(kind==RIPPLE_HTTP_ERROR){c->error_used=0;c->error[0]=0;}
        }
        n=got-i;if(n>c->record_left)n=c->record_left;
        if(c->record_kind==RIPPLE_HTTP_HEADERS) {
            unsigned end=i+n;
            for(;i<end&&!c->failure&&!c->aborted;i++) {
                if(c->headers_done||!c->header_budget--||c->line_used>=WEB_LINE+2){fail(c,"HTTP response headers exceed bounds");break;}
                if(buf[i]=='\n'){process_line(c);c->line_used=0;}
                else c->line[c->line_used++]=buf[i];
            }
        } else if(c->record_kind==RIPPLE_HTTP_BODY) {
            c->received+=n;
            if(c->received>WEB_BODY_LIMIT){fail(c,"HTTP response exceeds configured source limit");break;}
            memset(&message,0,sizeof(message));message.type=FETCH_DATA;
            message.data.header_or_data.buf=buf+i;message.data.header_or_data.len=n;
            if(!c->aborted)send_message(c,&message);i+=n;
        } else {
            unsigned end=i+n;
            for(;i<end;i++)if(c->error_used+1<sizeof(c->error)) {
                unsigned ch=buf[i];c->error[c->error_used++]=ch>=32&&ch<=126?ch:' ';
            }
            c->error[c->error_used]=0;
        }
        c->record_left-=n;
        if(!c->record_left) {
            if(c->record_kind==RIPPLE_HTTP_ERROR){fail(c,c->error[0]?c->error:"HTTP request failed");break;}
            c->record_used=0;
        }
    }
}
static void poll_fetch(lwc_string *scheme)
{
    struct web_fetch *c=jobs,*next;
    ns_octet_t buf[4096];ssize_t got;unsigned round,i;
    fetch_msg message;
    /* Reap idle children too; an idle server may close its connection, but
     * the worker normally stays alive to dial again for its next request. */
    for(i=0;i<WEB_WORKERS;i++)if(!workers[i].busy&&workers[i].pid>0&&reap_worker(&workers[i],false)) {
        stop_worker(&workers[i]);workers[i].pid=0;workers[i].origin[0]=0;
    }
    for(;c;c=next) {
        next=c->next;
        if(c->locked)continue;
        if(c->draining&&now_ms()-c->drain_at>2000UL) {
            c->draining=false;kill_worker(c);
        }
        if(c->aborted&&!c->draining){if(collect(c,false))complete(c,FETCH_FINISHED,NULL);continue;}
        if(!c->started)continue;
        drain_errors(c);
        if(!c->progress) {
            c->progress=true;memset(&message,0,sizeof(message));message.type=FETCH_PROGRESS;
            message.data.progress="Fetching through guest HTTP/TLS worker";send_message(c,&message);
        }
        if(c->aborted&&!c->draining)continue;
        if(now_ms()-c->started_at>240000UL)fail(c,"Network request exceeded 240 second deadline");
        if(c->worker&&!c->failure&&c->request_sent<c->request_size) {
            got=write(c->worker->input,c->request+c->request_sent,c->request_size-c->request_sent);
            if(got>0)c->request_sent+=got;
            else if(!got||(errno!=EAGAIN&&errno!=EINTR))fail(c,"HTTP worker request pipe failed");
        }
        for(round=0;round<8&&!c->eof&&(!c->aborted||c->draining)&&!c->failure;round++) {
            got=read(c->worker->output,buf,sizeof(buf));
            if(!got){
                drain_errors(c);collect(c,false);
                if(getenv("NETSURF_TRACE_HTTP"))fprintf(stderr,"NETSURF_HTTP_EXIT pid=%d status=%d reaped=%d detail=%s\n",
                    c->worker->pid,c->worker->status,c->worker->exited,c->error);
                fail(c,"HTTP worker exited before completing the response");break;
            }
            if(got<0){if(errno!=EAGAIN&&errno!=EINTR)fail(c,"HTTP worker output pipe failed");break;}
            response_data(c,buf,got);
        }
        if((c->eof||c->failure)&&collect(c,false)) {
            long code=fetch_http_code(c->parent);
            if(c->aborted)complete(c,FETCH_FINISHED,NULL);
            else if(c->failure)complete(c,FETCH_ERROR,c->failure);
            else if(code==301||code==302||code==303||code==307||code==308) {
                nsurl *target=NULL;
                if(c->location&&nsurl_join(c->url,c->location,&target)==NSERROR_OK) {
                    complete(c,FETCH_REDIRECT,nsurl_access(target));nsurl_unref(target);
                } else complete(c,FETCH_ERROR,"Invalid or missing redirect Location");
            } else if(code==304)complete(c,FETCH_NOTMODIFIED,NULL);
            else if(c->only_2xx&&(code<200||code>=300))complete(c,FETCH_ERROR,"HTTP status is not successful");
            else complete(c,FETCH_FINISHED,NULL);
        }
    }
}
static int fdset(lwc_string *scheme,fd_set *reads,fd_set *writes,fd_set *errors)
{
    int max=-1;struct web_fetch *c;struct web_worker *w;
    for(c=jobs;c;c=c->next)if(c->started&&(!c->aborted||c->draining)&&(w=c->worker)!=NULL) {
        if(w->output>=0){FD_SET(w->output,reads);if(w->output>max)max=w->output;}
        if(w->errors>=0){FD_SET(w->errors,reads);if(w->errors>max)max=w->errors;}
        if(w->input>=0&&c->request_sent<c->request_size){FD_SET(w->input,writes);if(w->input>max)max=w->input;}
    }
    return max;
}
static void finalise(lwc_string *scheme)
{
    unsigned i;
    while(jobs)destroy(jobs);
    for(i=0;i<WEB_WORKERS;i++){stop_worker(&workers[i]);reap_worker(&workers[i],true);workers[i].pid=0;}
    free(ca_path);ca_path=NULL;
}
nserror netsurf_http_register(const char *ca_file)
{
    static const struct fetcher_operation_table operations={
        .initialise=initialise,.acceptable=acceptable,.setup=setup,.start=start,
        .abort=abort_fetch,.free=destroy,.poll=poll_fetch,.fdset=fdset,.finalise=finalise,
    };
    const char *names[2]={"http","https"};unsigned i;nserror error;
    web_body_limit=ns_image_limit("NETSURF_IMAGE_INPUT",NS_IMAGE_INPUT_DEFAULT,33554432UL);
    web_worker_limit=ns_image_limit("NETSURF_HTTP_WORKERS",2,WEB_WORKERS);
    if(!web_body_limit||!web_worker_limit)return NSERROR_BAD_PARAMETER;
    signal(SIGPIPE,SIG_IGN);
    if(ca_file){ca_path=strdup(ca_file);if(!ca_path)return NSERROR_NOMEM;}
    for(i=0;i<2;i++) {
        lwc_string *scheme=NULL;
        if(lwc_intern_string(names[i],strlen(names[i]),&scheme)!=lwc_error_ok)return NSERROR_NOMEM;
        error=fetcher_add(scheme,&operations);
        if(error!=NSERROR_OK){lwc_string_unref(scheme);return error;}
    }
    return NSERROR_OK;
}
