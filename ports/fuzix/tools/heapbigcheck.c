/* Hosted private-heap integration: large bins, user copies, fork and exec. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <setjmp.h>
#include <sys/wait.h>
#include "../../../fuse-x/include/memory-layout.h"
#define C(x) do {if(!(x)){printf("HEAP_BIG_FAIL: %s errno=%d\n",#x,errno);return 1;}}while(0)
#define LARGE (32UL*1024UL*1024UL) /* 64 MiB physical storage in C16. */
static unsigned long offsets[]={0,63999,64000,12288000UL,LARGE-1};
static void jump(jmp_buf *b) {longjmp(*b,42);}
int main(int argc,char **argv) {
    char *p,*q,*r,argument[40],*args[4];pid_t pid;int status,fd[2],j;unsigned i;
    jmp_buf *b;
    if(argc==3&&!strcmp(argv[1],"after")) {
        /* Exec must drop the child's previous private extension, not retain
         * its distant committed page or clear the parent's COW page. */
        p=(void *)strtoul(argv[2],NULL,10);C((unsigned char)*p==0);
        q=malloc(1024);C(q!=NULL);q[1023]=0xabcd;free(q);
        puts("HEAP_BIG_EXEC_OK");return 0;
    }
    /* Bootstrap and previous-generation heaps remain usable. */
    C(brk((void *)FUSE_LEGACY_HEAP_BASE)==0);
    r=sbrk(64004UL);C(r==(void *)FUSE_LEGACY_HEAP_BASE);r[64003UL]=0xfeed;
    p=malloc(LARGE);C(p!=NULL);C(((unsigned long)p>>16)>=FUSE_HEAP_FIRST_BANK);
    C((unsigned char)r[64003UL]==0xfeed);
    for(i=0;i<5;i++){C(p[offsets[i]]==0);p[offsets[i]]=0xa000U+i;}
    C(pipe(fd)==0);
    C(write(fd[1],p+63999,2)==2);C(read(fd[0],p+127999,2)==2);
    C((unsigned char)p[127999]==0xa001&&(unsigned char)p[128000]==0xa002);
    errno=0;
    C(write(fd[1],(void *)(((FUSE_HEAP_END_BANK-1)<<16)|63999U),2)==-1&&errno==EFAULT);
    C(close(fd[0])==0&&close(fd[1])==0);
    b=malloc(sizeof(*b));C(b!=NULL);j=setjmp(*b);if(!j)jump(b);C(j==42);free(b);
    pid=fork();C(pid>=0);
    if(!pid) {
        for(i=0;i<5;i++)C((unsigned char)p[offsets[i]]==0xa000U+i);
        for(i=0;i<5;i++)p[offsets[i]]=0xb000U+i;
        args[0]="missing";args[1]=NULL;
        C(execv("/bin/no-such-heap-app",args)==-1&&errno==ENOENT);
        C((unsigned char)p[LARGE-1]==0xb004);
        snprintf(argument,sizeof(argument),"%lu",(unsigned long)(p+LARGE-1));
        args[0]="heapbig";args[1]="after";args[2]=argument;args[3]=NULL;
        execv("/bin/heapbig",args);return 2;
    }
    C(waitpid(pid,&status,0)==pid&&WIFEXITED(status)&&WEXITSTATUS(status)==0);
    for(i=0;i<5;i++)C((unsigned char)p[offsets[i]]==0xa000U+i);
    q=realloc(p,40UL*1024UL*1024UL);C(q!=NULL);
    for(i=0;i<5;i++)C((unsigned char)q[offsets[i]]==0xa000U+i);
    free(q);
    p=malloc(40UL*1024UL*1024UL);C(p!=NULL);p[40UL*1024UL*1024UL-1]=0xcafe;free(p);
    C(calloc(~0UL,2)==NULL);
    C(malloc(FUSE_HEAP_CELLS)==NULL);
    puts("HEAP_BIG_OK");return 0;
}
