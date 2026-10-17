/* Exercise independently loaded code across fork and replacement by exec. */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>
#include <sys/wait.h>
static int global=7;
static int twice(int value) { return 2*value; }
static int (*callback)(int)=twice;
static void require(int condition,const char *message) {
    if(!condition) {fprintf(stderr,"IMAGE CHECK FAILED: %s (errno %d)\n",message,errno);exit(1);}
}
int main(int argc,char **argv) {
    int child,status,fd,local=11,n,i;
    char byte,pid[8],*next[4],*environment[2];
    unsigned char buffer[257];
    unsigned long position=0;
    char *heap;
    require(global==7 && callback(9)==18,"initial data/function relocation");
    if(argc>1 && !strcmp(argv[1],"--parent")) {
        require(getenv("FUSE_IMAGE_TEST") && !strcmp(getenv("FUSE_IMAGE_TEST"),"yes"),"exec environment");
        child=atoi(argv[2]);
        require(waitpid(child,&status,0)==child && status==(17<<8),"old image survives parent exec");
        puts("Fuse-X: independent image checks passed");
        return 0;
    }
    heap=malloc(70000UL);
    require(heap!=NULL,"bank-spanning heap allocation");
    heap[0]='a';heap[65000UL]='b';heap[69999UL]='c';
    require(heap[0]=='a' && heap[65000UL]=='b' && heap[69999UL]=='c',"heap bank addressing");
    free(heap);
    fd=open("/tmp/large-copy",O_RDONLY);
    require(fd>=0,"large copied file");
    while((n=read(fd,buffer,sizeof(buffer)))>0)
        for(i=0;i<n;i++) require(buffer[i]==position++%251UL,"large binary copy contents");
    require(n==0 && position==70000UL,"large binary copy length");close(fd);
    fd=open("/tmp/image-offset",O_RDWR|O_CREAT|O_TRUNC,0600);
    require(fd>=0 && write(fd,"abcd",4)==4 && lseek(fd,0,SEEK_SET)==0,"file setup");
    child=fork();require(child>=0,"fork");
    if(!child) {
        global=99;local=22;
        require(read(fd,&byte,1)==1 && byte=='a',"child shared descriptor");
        _exit(global==99 && local==22 && callback(6)==12 ? 23 : 1);
    }
    require(waitpid(child,&status,0)==child && status==(23<<8),"fork wait status");
    require(global==7 && local==11 && read(fd,&byte,1)==1 && byte=='b',"COW and shared offset");close(fd);
    global=31;
    next[0]=argv[0];next[1]=NULL;
    require(execv("/bin/bad-abi",next)<0 && errno==ENOEXEC && global==31 && callback(4)==8,"failed exec preserves image");
    require(execve("/bin/outside",(char *const *)0xf00000UL,environ)<0 && errno==EFAULT && global==31,"invalid argv preserves image and discards staged code");
    child=fork();require(child>=0,"second fork");
    if(!child) {
        sleep(1);
        require(global==31 && local==11 && callback(8)==16,"old code and globals remain live");
        _exit(17);
    }
    sprintf(pid,"%d",child);
    next[0]=argv[0];next[1]="--parent";next[2]=pid;next[3]=NULL;
    environment[0]="FUSE_IMAGE_TEST=yes";environment[1]=NULL;
    execve("/bin/image-check",next,environment);
    require(0,"replace current executable");
    return 1;
}
