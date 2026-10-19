/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/wait.h>
#define C(x) do {if(!(x)){printf("entropycheck FAILED line %u errno %d\n",__LINE__,errno);return 1;}}while(0)
int main(void) {
    unsigned char *arena,*p,other[256],child[32];
    int fd,pid,pipefd[2],status;unsigned i;
    arena=malloc(130000UL);C(arena!=NULL);
    p=(void *)((((unsigned long)arena>>16)+1)<<16);p-=8;
    if(p<arena){p+=64000UL;}
    memset(p-1,0x5aa5,302);
    fd=open("/dev/urandom",O_RDONLY|O_CLOEXEC);C(fd>=0);
    C(read(fd,p,300)==256);
    C(p[-1]==0x5aa5&&p[256]==0x5aa5&&p[300]==0x5aa5);
    for(i=0;i<256;i++)C(p[i]<=255);
    C(read(fd,other,sizeof(other))==sizeof(other));
    /* Independence smoke check, not a statistical test of the OS generator. */
    C(memcmp(p,other,256)!=0);
    errno=0;C(read(fd,(void *)0,16)==-1&&errno==EFAULT);
    C(pipe(pipefd)==0);pid=fork();C(pid>=0);
    if(!pid){
        close(pipefd[0]);
        if(read(fd,p,32)!=32||write(pipefd[1],p,32)!=32)_exit(1);
        _exit(0);
    }
    close(pipefd[1]);C(read(pipefd[0],child,32)==32);close(pipefd[0]);
    C(waitpid(pid,&status,0)==pid&&status==0);
    C(memcmp(child,p,32)!=0);
    C(p[-1]==0x5aa5&&p[256]==0x5aa5&&p[300]==0x5aa5);
    close(fd);free(arena);puts("entropycheck passed");return 0;
}
