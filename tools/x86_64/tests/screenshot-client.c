#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define SCREENSHOT_HOST_TEST 1
#define main screenshot_main
#include "../graphics/screenshot.c"
#undef main
static unsigned fail_row,width=2,height=2;
unsigned long riplix_screen(unsigned op,unsigned y,char *data,unsigned long cap){
    if(op==0)return width;if(op==1)return height;
    if(y==fail_row || cap<width*3)return 0xffffffffUL;
    for(unsigned i=0;i<width*3;i++)data[i]=(char)(y*width*3+i);return width*3;
}
int main(int argc,char **argv){
    assert(argc==2);char *args[]={"screenshot",argv[1]};fail_row=10;
    assert(!screenshot_main(2,args));FILE *f=fopen(argv[1],"rb");assert(f);
    unsigned char bytes[100];size_t n=fread(bytes,1,sizeof(bytes),f);fclose(f);
    const char *header="P6\n2 2\n255\n";assert(n==strlen(header)+12 && !memcmp(bytes,header,strlen(header)));
    for(unsigned i=0;i<12;i++)assert(bytes[strlen(header)+i]==i);
    fail_row=1;assert(screenshot_main(2,args)==1);assert(!fopen(argv[1],"rb"));
    width=5;height=3;fail_row=10;
    char *scaled[]={"screenshot","--scale","2",argv[1]};
    assert(!screenshot_main(4,scaled));f=fopen(argv[1],"rb");assert(f);
    n=fread(bytes,1,sizeof(bytes),f);fclose(f);header="P6\n3 2\n255\n";
    assert(n==strlen(header)+18 && !memcmp(bytes,header,strlen(header)));
    for(unsigned y=0;y<2;y++)for(unsigned x=0;x<3;x++)for(unsigned c=0;c<3;c++)
        assert(bytes[strlen(header)+(y*3+x)*3+c]==(y*2*5+x*2)*3+c);
    scaled[2]="0";assert(screenshot_main(4,scaled)==2);
    scaled[2]="17";assert(screenshot_main(4,scaled)==2);
    scaled[2]="2x";assert(screenshot_main(4,scaled)==2);
    remove(argv[1]);
    puts("Screenshot checks passed: full-size RGB octets, failed-output cleanup, odd-size nearest sampling and scale bounds");
}
