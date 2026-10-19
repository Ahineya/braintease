/* SPDX-License-Identifier: GPL-2.0-only */
/* Asynchronous NetSurf adapter to the generic guest htget --response CLI.
 * Standard pipes carry HTTP entity headers/body and diagnostics. TLS/crypto
 * objects live entirely in the exec'd worker, never in the browser ABI. */
#include <netsurf_profile.h>
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
#define WEB_HEADERS 4
#define WEB_LINE 1024
#define WEB_BODY_LIMIT 262144UL
#define WEB_WORKERS 2
struct web_fetch {
    struct web_fetch *next;
    struct fetch *parent;
    nsurl *url;
    pid_t pid;
    int output,errors,status;
    bool started,aborted,locked,eof,exited,headers_done,progress,only_2xx,known;
    unsigned line_used,header_budget,header_count,error_used;
    unsigned long received,total,started_at;
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
static void kill_worker(struct web_fetch *c)
{
    close_pipe(&c->output); close_pipe(&c->errors);
    if(c->pid>0&&!c->exited)kill(c->pid,SIGKILL);
}
static bool collect(struct web_fetch *c,bool block)
{
    pid_t result;
    if(c->pid<=0||c->exited)return true;
    do { result=waitpid(c->pid,&c->status,block?0:WNOHANG); } while(result<0&&errno==EINTR&&block);
    if(result==c->pid) { c->exited=true; return true; }
    if(result<0&&errno==ECHILD) { c->status=0x100; c->exited=true; return true; }
    return false;
}
static void destroy(void *opaque)
{
    struct web_fetch *c=opaque,**at=&jobs;
    unsigned i;
    while(*at&&*at!=c)at=&(*at)->next;
    if(*at)*at=c->next;
    if(c->started)--active_count;
    kill_worker(c); collect(c,true);
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
    c->parent=parent; c->url=nsurl_ref(url); c->output=c->errors=-1; c->only_2xx=only_2xx;
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
static bool start(void *opaque)
{
    struct web_fetch *c=opaque;
    int out[2]={-1,-1},err[2]={-1,-1},fd;
    char *cookie;
    if(active_count==WEB_WORKERS)return false;
    ++active_count; c->started=true; c->started_at=now_ms();
    if(c->aborted||c->failure) { c->eof=true;return true; }
    cookie=urldb_get_cookie(c->url,true);
    if(cookie) {
        size_t n=strlen(cookie);
        if(c->header_count==WEB_HEADERS||n>WEB_LINE-8)fail(c,"Cookie exceeds this browser's request bounds");
        else {
            char *s=malloc(n+9);
            if(s) { strcpy(s,"Cookie: ");strcat(s,cookie);c->headers[c->header_count++]=s; }
            else fail(c,"Unable to allocate cookie header");
        }
        free(cookie);
    }
    if(c->failure)return true;
    if(pipe(out)<0||pipe(err)<0)goto failed;
    c->pid=fork();
    if(c->pid<0)goto failed;
    if(!c->pid) {
        char *args[16]; unsigned count=0,i;
        if(dup2(out[1],1)<0||dup2(err[1],2)<0)_exit(126);
        /* Never keep the browser's Nano-X/socket/files alive after exec. */
        for(fd=3;fd<FD_SETSIZE;fd++)close(fd);
        args[count++]="htget";args[count++]="--response";
        if(ca_path){args[count++]="--cacert";args[count++]=ca_path;}
        for(i=0;i<c->header_count;i++){args[count++]="--header";args[count++]=c->headers[i];}
        args[count++]=(char *)nsurl_access(c->url);args[count++]="-";args[count]=NULL;
        execv("/bin/htget",args);
        write(2,"Unable to execute /bin/htget\n",sizeof("Unable to execute /bin/htget\n")-1);_exit(127);
    }
    close(out[1]);out[1]=-1;close(err[1]);err[1]=-1;
    c->output=out[0];c->errors=err[0];
    if(fcntl(c->output,F_SETFL,O_NONBLOCK)<0||fcntl(c->errors,F_SETFL,O_NONBLOCK)<0)fail(c,"Unable to configure fetch pipes");
    return true;
failed:
    for(fd=0;fd<2;fd++){if(out[fd]>=0)close(out[fd]);if(err[fd]>=0)close(err[fd]);}
    c->pid=0;fail(c,"Unable to start guest HTTP worker");return true;
}
static void abort_fetch(void *opaque)
{
    struct web_fetch *c=opaque;
    c->aborted=true;kill_worker(c);
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
                    fail(c,"HTTP response exceeds 256 KiB source limit");return;
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
    if(c->errors<0)return;
    got=read(c->errors,buf,sizeof(buf));
    if(got>0)for(i=0;i<(unsigned)got;i++) {
        unsigned ch=(unsigned char)buf[i];
        if(c->error_used+1<sizeof(c->error)&&ch>=32&&ch<=126)c->error[c->error_used++]=ch;
        else if((ch=='\n'||ch=='\r')&&c->error_used&&c->error[c->error_used-1]!=' '&&c->error_used+1<sizeof(c->error))c->error[c->error_used++]=' ';
        c->error[c->error_used]=0;
    }
    else if(got==0)close_pipe(&c->errors);
    else if(errno!=EAGAIN&&errno!=EINTR) { close_pipe(&c->errors); fail(c,"HTTP worker diagnostic pipe failed"); }
}
static void poll_fetch(lwc_string *scheme)
{
    struct web_fetch *c=jobs,*next;
    ns_octet_t buf[2048];ssize_t got;unsigned round,i;
    fetch_msg message;
    for(;c;c=next) {
        next=c->next;
        if(c->locked)continue;
        if(c->aborted) { if(collect(c,false))complete(c,FETCH_FINISHED,NULL);continue; }
        if(!c->started)continue;
        drain_errors(c);
        if(!c->progress) {
            c->progress=true;memset(&message,0,sizeof(message));message.type=FETCH_PROGRESS;
            message.data.progress="Fetching through guest HTTP/TLS worker";send_message(c,&message);
        }
        if(c->aborted)continue;
        if(now_ms()-c->started_at>240000UL)fail(c,"Network request exceeded 240 second deadline");
        for(round=0;round<4&&!c->eof&&!c->aborted&&!c->failure;round++) {
            got=read(c->output,buf,sizeof(buf));
            if(!got) { c->eof=true;close_pipe(&c->output);break; }
            if(got<0) { if(errno!=EAGAIN&&errno!=EINTR)fail(c,"HTTP worker output pipe failed");break; }
            for(i=0;i<(unsigned)got&&!c->headers_done&&!c->failure&&!c->aborted;i++) {
                if(!c->header_budget--||c->line_used>=WEB_LINE+1) { fail(c,"HTTP response headers exceed bounds");break; }
                if(buf[i]=='\n') { process_line(c);c->line_used=0; }
                else c->line[c->line_used++]=buf[i];
            }
            if(c->headers_done&&!c->failure&&!c->aborted&&i<(unsigned)got) {
                c->received+=(unsigned)got-i;
                if(c->received>WEB_BODY_LIMIT) { fail(c,"HTTP response exceeds 256 KiB source limit");break; }
                memset(&message,0,sizeof(message));message.type=FETCH_DATA;
                message.data.header_or_data.buf=buf+i;message.data.header_or_data.len=(unsigned)got-i;
                send_message(c,&message);
            }
        }
        if(c->failure&&!c->eof)kill_worker(c);
        if((c->eof||c->failure)&&collect(c,false)) {
            long code=fetch_http_code(c->parent);
            drain_errors(c);
            if(c->aborted)complete(c,FETCH_FINISHED,NULL);
            else if(c->failure||!WIFEXITED(c->status)||WEXITSTATUS(c->status)!=0||!c->headers_done)
                complete(c,FETCH_ERROR,c->failure?c->failure:c->error[0]?c->error:"HTTP worker failed or response was incomplete");
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
    int max=-1;
    struct web_fetch *c;
    for(c=jobs;c;c=c->next)if(c->started&&!c->aborted) {
        if(c->output>=0){FD_SET(c->output,reads);if(c->output>max)max=c->output;}
        if(c->errors>=0){FD_SET(c->errors,reads);if(c->errors>max)max=c->errors;}
    }
    return max;
}
static void finalise(lwc_string *scheme)
{
    while(jobs)destroy(jobs);
    free(ca_path);ca_path=NULL;
}
nserror netsurf_http_register(const char *ca_file)
{
    static const struct fetcher_operation_table operations={
        .initialise=initialise,.acceptable=acceptable,.setup=setup,.start=start,
        .abort=abort_fetch,.free=destroy,.poll=poll_fetch,.fdset=fdset,.finalise=finalise,
    };
    const char *names[2]={"http","https"};unsigned i;nserror error;
    if(ca_file){ca_path=strdup(ca_file);if(!ca_path)return NSERROR_NOMEM;}
    for(i=0;i<2;i++) {
        lwc_string *scheme=NULL;
        if(lwc_intern_string(names[i],strlen(names[i]),&scheme)!=lwc_error_ok)return NSERROR_NOMEM;
        error=fetcher_add(scheme,&operations);
        if(error!=NSERROR_OK){lwc_string_unref(scheme);return error;}
    }
    return NSERROR_OK;
}
