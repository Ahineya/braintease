/* A local-file validator must round-trip the guest's actual 32-bit mtime. */
#define main core_acceptance_main
#include "core.c"
#undef main
#include "content/llcache.h"
#include <sys/stat.h>
static bool finished,failed;
static nserror event(llcache_handle *h,const llcache_event *e,void *pw) {
    if(e->type==LLCACHE_EVENT_DONE)finished=true;
    if(e->type==LLCACHE_EVENT_ERROR)finished=failed=true;
    return NSERROR_OK;
}
int main(int argc,char **argv) {
    struct stat st;nsurl *url=NULL;llcache_handle *handles[3]={NULL,NULL,NULL};
    char expected[40];unsigned i;const char *etag;size_t lengths[3];const ns_octet_t *sources[3];
    check(netsurf_register(&tables)==NSERROR_OK,"frontend");
    check(nslog_init(NULL,&argc,argv)==NSERROR_OK,"logging");
    check(nsoption_init(NULL,NULL,NULL)==NSERROR_OK,"options");
    check(netsurf_init(NULL)==NSERROR_OK,"core");
    check(stat("/fixture.txt",&st)==0,"file metadata");
    snprintf(expected,sizeof(expected),"\"%10ld\"",(long)st.st_mtime);
    check(nsurl_create("file:///fixture.txt",&url)==NSERROR_OK,"file URL");
    for(i=0;i<3;i++) {
        if(i==2) {
            FILE *file=fopen("/fixture.txt","w");
            check(file&&fwrite("changed",1,7,file)==7,"replace file contents");
            if(file)check(fclose(file)==0,"close changed file");
            check(stat("/fixture.txt",&st)==0&&st.st_mtime!=0,"changed file timestamp");
            snprintf(expected,sizeof(expected),"\"%10ld\"",(long)st.st_mtime);
        }
        uint64_t start,now;finished=failed=false;
        check(llcache_handle_retrieve(url,0,NULL,NULL,event,NULL,&handles[i])==NSERROR_OK,"retrieve");
        nsu_getmonotonic_ms(&start);
        do {poll_callbacks();usleep(2000);nsu_getmonotonic_ms(&now);}while(!finished&&now-start<20000);
        check(finished&&!failed,"retrieval completed");
        etag=llcache_handle_get_header(handles[i],"ETag");
        fprintf(stderr,"FILE_ETAG actual=%s expected=%s\n",etag?etag:"(missing)",expected);
        check(etag&&!strcmp(etag,expected),"mtime validator round trip");
        sources[i]=llcache_handle_get_source_data(handles[i],&lengths[i]);
    }
    check(lengths[0]==6&&lengths[1]==6&&!memcmp(sources[1],"cached",6),"file body");
    check(sources[0]==sources[1],"not-modified validation shares the existing source");
    check(lengths[2]==7&&!memcmp(sources[2],"changed",7),"changed file gets new source");
    check(!memcmp(sources[0],"cached",6),"pinned old source remains intact");
    for(i=0;i<3;i++)llcache_handle_release(handles[i]);nsurl_unref(url);
    netsurf_exit();nsoption_finalise(NULL,NULL);nslog_finalise();
    printf("NETSURF_FILECACHE checks=%d failures=%d\n",checks,failures);return failures?1:0;
}
