/* SPDX-License-Identifier: MIT */
/* Compile with libraries/image/decode.c; time guest decoding, not host codecs. */
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include "../image/image.h"
static unsigned long micros(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC,&ts);
    return (unsigned long)ts.tv_sec*1000000UL+(unsigned long)ts.tv_nsec/1000UL;
}
int main(int argc,char **argv)
{
    FILE *f;RiByte *data;long length;unsigned i,count;
    unsigned long start,sum=0;
    if(argc!=3||(count=atoi(argv[2]))==0||count>10000||!(f=fopen(argv[1],"rb")))return 1;
    if(fseek(f,0,SEEK_END)||(length=ftell(f))<=0||fseek(f,0,SEEK_SET))return 2;
    data=malloc(length);
    if(!data||fread(data,1,length,f)!=(size_t)length)return 2;
    fclose(f);start=micros();
    for(i=0;i<count;i++) {
        RiImage *im=NULL;int status=ri_image_decode(data,length,NULL,&im);
        if(status||!im){printf("DECODE error=%d\n",status);free(data);return 3;}
        sum+=im->rgba[0]+im->rgba[((size_t)im->width*im->height-1)*4];
        ri_image_free(im);
    }
    printf("DECODE count=%u us=%lu checksum=%lu\n",count,micros()-start,sum);
    free(data);return 0;
}
