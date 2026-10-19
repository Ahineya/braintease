#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
static int global=7;
static int twice(int n){return n*2;}
static int (*callback)(int)=twice;
static void require(int condition,const char *message) {
    if(!condition){fprintf(stderr,"PORTABLE_PROCESS_FAILED: %s (%d)\n",message,errno);exit(1);}
}
struct Returned {unsigned words[13];};
static struct Returned returned(unsigned n) {
    struct Returned value;unsigned j;
    for(j=0;j<13;j++)value.words[j]=n+j;
    return value;
}
int main(int argc,char **argv,char **envp) {
    int child,status,local=11;char *heap;
    char *next[3];
    require(global==7 && callback(9)==18,"initial relocations");
    require(envp!=NULL,"environment startup");
    if(argc>1 && !strcmp(argv[1],"--child"))return 17;
    {
        unsigned n,j;
        for(n=0;n<5000;n++){
            struct Returned value=returned(n);
            for(j=0;j<13;j++)require(value.words[j]==n+j,"aggregate return across timer checkpoints");
        }
    }
    heap=malloc(70000UL);require(heap!=NULL,"large allocation");
    heap[0]='a';heap[65000UL]='b';heap[69999UL]='c';
    require(heap[0]=='a' && heap[65000UL]=='b' && heap[69999UL]=='c',"bank crossing");
    free(heap);
    child=fork();require(child>=0,"fork");
    if(!child){global=99;local=22;_exit(global==99 && local==22 && callback(3)==6?23:1);}
    require(waitpid(child,&status,0)==child && status==(23<<8),"fork status");
    require(global==7 && local==11 && callback(4)==8,"parent state");
    child=fork();require(child>=0,"exec fork");
    if(!child){next[0]=argv[0];next[1]="--child";next[2]=NULL;execv(argv[0],next);_exit(1);}
    require(waitpid(child,&status,0)==child && status==(17<<8),"independent exec");
    puts("portable hosted process checks passed");return 0;
}
