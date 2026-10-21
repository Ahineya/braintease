/* SPDX-License-Identifier: Zlib */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "../deflate/gzip.h"
#include <gzip-fixtures.h>
struct stream {const unsigned char *data;uint32_t size,at,hash,total;unsigned fail;};
static unsigned failures,checks;
#define C(x) do{checks++;if(!(x)){printf("gzip failed line %u\n",__LINE__);failures++;}}while(0)
static int next(void *opaque) {
    struct stream *s=opaque;
    return s->at==s->size?-1:s->data[s->at++];
}
static int emit(void *opaque,const unsigned char *data,unsigned n) {
    struct stream *s=opaque;unsigned i;
    if(s->fail)return -1;
    C(n>0&&n<=4096);
    for(i=0;i<n;i++){s->hash^=data[i];s->hash*=16777619UL;}
    s->total+=n;return 0;
}
int main(void) {
    unsigned i;uint32_t produced,cut;int32_t r;struct stream s;
    for(i=0;i<sizeof(gz_vectors)/sizeof(gz_vectors[0]);i++) {
        const struct gzip_vector *v=gz_vectors+i;
        memset(&s,0,sizeof(s));s.data=v->data;s.size=v->size;s.hash=2166136261UL;
        produced=0;
        r=ri_gzip_stream(next,emit,&s,v->size,v->length,&produced);
        C(r==TINF_OK&&produced==v->length&&s.total==v->length&&s.hash==v->hash&&s.at==v->size);
        for(cut=0;cut<4;cut++) {
            s.at=s.total=0;s.size=cut==0?0:cut==1?9:cut==2?v->size/2:v->size-1;produced=123;
            C(ri_gzip_stream(next,emit,&s,v->size,v->length,&produced)<0&&produced==123);
        }
        s.at=s.total=0;s.size=v->size;produced=123;
        C(ri_gzip_stream(next,emit,&s,v->size-1,v->length,&produced)<0&&produced==123);
        s.at=s.total=0;
        C(ri_gzip_stream(next,emit,&s,v->size,v->length-1,&produced)<0&&produced==123);
        s.at=s.total=0;s.fail=1;
        C(ri_gzip_stream(next,emit,&s,v->size,v->length,&produced)<0&&produced==123);
    }
    for(i=0;i<sizeof(gz_invalid)/sizeof(gz_invalid[0]);i++) {
        const struct gzip_vector *v=gz_invalid+i;
        memset(&s,0,sizeof(s));s.data=v->data;s.size=v->size;produced=123;
        C(ri_gzip_stream(next,emit,&s,v->size,v->length,&produced)<0&&produced==123);
    }
    printf("GZIP_CHECK checks=%u failures=%u\n",checks,failures);return failures?1:0;
}
