/* SPDX-License-Identifier: MIT */
/* Fail each service-model allocation in turn, including vector growth, graph
 * construction and profile parsing. Every partial model must be releasable. */
#include "../service.h"
#include <assert.h>
static size_t fail_at,calls,live;
static void *test_malloc(size_t size){
    void *p;if(++calls==fail_at)return NULL;p=malloc(size);if(p)live++;return p;
}
static void *test_calloc(size_t count,size_t size){
    void *p;if(++calls==fail_at)return NULL;p=calloc(count,size);if(p)live++;return p;
}
static void *test_realloc(void *old,size_t size){
    void *p;int fresh=old==NULL;if(++calls==fail_at)return NULL;
    p=realloc(old,size);if(p&&fresh)live++;return p;
}
static void test_free(void *p){if(p){assert(live);live--;free(p);}}
#define malloc test_malloc
#define calloc test_calloc
#define realloc test_realloc
#define free test_free
#include "../config.c"
#undef malloc
#undef calloc
#undef realloc
#undef free
static int attempt(const char *directory,const char *profile){
    struct services model;char error[512];int result;
    calls=0;result=sv_load(&model,directory,error,sizeof(error));
    if(!result)result=sv_profile(&model,profile,error,sizeof(error));
    if(result)assert(strstr(error,"out of memory"));
    sv_free(&model);assert(!live);return result;
}
int main(int argc,char **argv){
    size_t total,i;assert(argc==3);
    assert(!attempt(argv[1],argv[2]));total=calls;
    for(i=1;i<=total;i++){fail_at=i;assert(attempt(argv[1],argv[2]));}
    printf("SERVICES_MEMORY_OK: %lu allocation failure points cleaned up\n",(unsigned long)total);return 0;
}
