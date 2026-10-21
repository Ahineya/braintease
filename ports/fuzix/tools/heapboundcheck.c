/* The freestanding heap profile must reach libc as well as this application. */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include "../../../fuse-x/include/memory-layout.h"
#define C(x) do {if(!(x)){printf("HEAP_BOUND_FAIL: %s errno=%d\n",#x,errno);return 1;}}while(0)
int main(void) {
    char *p=malloc(16);int fd[2];
    C(FUSE_HEAP_FIRST_BANK==48&&FUSE_HEAP_END_BANK==240);
    C(p!=NULL&&((unsigned long)p>>16)>=48&&((unsigned long)p>>16)<240);
    C(pipe(fd)==0);errno=0;
    C(write(fd[1],(void *)(256UL<<16),1)==-1&&errno==EFAULT);
    C(write(fd[1],(void *)((239UL<<16)|63999U),2)==-1&&errno==EFAULT);
    errno=0;C(brk((void *)(256UL<<16))==-1&&errno==ENOMEM);
    C(close(fd[0])==0&&close(fd[1])==0);free(p);
    puts("HEAP_BOUND_OK");return 0;
}
