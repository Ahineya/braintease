/* SPDX-License-Identifier: MIT */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../deflate/deflate.h"
struct stream { const unsigned char *data,*expected;unsigned long size,length; };
#include <deflate-fixtures.h>
static unsigned checks,failures;
static void check(int value) {checks++;if(!value)failures++;}
int main(void)
{
    unsigned i,j;uint32_t length;int status;
    for(i=0;i<sizeof(streams)/sizeof(streams[0]);i++) {
        const struct stream *s=&streams[i];
        unsigned char *out=malloc(s->length+8),*trailing=malloc(s->size+1);
        if(!out||!trailing)return 2;
        memset(out,0xa5,s->length+8);length=s->length;
        check(ri_deflate(out,&length,s->data,s->size)==TINF_OK);
        check(length==s->length&&!memcmp(out,s->expected,length));
        for(j=0;j<8;j++)check(out[s->length+j]==0xa5);
        for(j=0;j<4;j++) {
            length=s->length;
            status=ri_deflate(out,&length,s->data,j==0?0:j==1?1:j==2?s->size/2:s->size-1);
            check(status!=TINF_OK&&length==s->length);
        }
        memcpy(trailing,s->data,s->size);trailing[s->size]=0;
        length=s->length;check(ri_deflate(out,&length,trailing,s->size+1)!=TINF_OK);
        if(s->length) {
            length=s->length-1;out[length]=0xa5;
            check(ri_deflate(out,&length,s->data,s->size)!=TINF_OK&&length==s->length-1);
            check(out[length]==0xa5);
        }
        free(out);free(trailing);
    }
    printf("DEFLATE_CHECK checks=%u failures=%u streams=%u\n",checks,failures,(unsigned)(sizeof(streams)/sizeof(streams[0])));
    return failures?1:0;
}
