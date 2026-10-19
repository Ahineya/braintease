/* Large data, relocations, syscall spans, COW and exec in a real guest. */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
static unsigned data[140003] = {[0]=17, [63999L]=18, [64000L]=19,
                               [128000L]=20, [140002L]=21};
static unsigned long wide[35003];
static unsigned *edge = data + 64000L;
static unsigned *tail = data + 140002L;
static unsigned count;
static void require(int okay, const char *why) {
    if (!okay) { fprintf(stderr,"GLOBAL_CHECK_FAILED %s errno=%d\n",why,errno); exit(1); }
    count++;
}
int main(int argc, char **argv) {
    unsigned long i;
    unsigned long crossing, start;
    union { unsigned *pointer; unsigned cells[2]; } address;
    int pid, status, fd;
    unsigned *heap;
    require(data[0]==17 && data[63999L]==18 && *edge==19 &&
            data[128000L]==20 && *tail==21, "sparse initialized data and pointers");
    require(data[64001L]==0 && wide[35002L]==0, "zero data");
    if (argc==2 && !strcmp(argv[1],"again")) {
        require(getenv("GLOBAL_BANK_TEST") && !strcmp(getenv("GLOBAL_BANK_TEST"),"survived"),"exec environment");
        puts("GLOBAL_BANKS_EXEC_OK"); return 0;
    }
    require(argc==1 && argv[0] && *argv[0], "arguments");
    require(putenv("GLOBAL_BANK_TEST=survived")==0,"environment setup");
    for (i=0;i<35003UL;i++) wide[i] = 0x12340000UL + i;
    for (i=0;i<35003UL;i++) require(wide[i]==0x12340000UL+i,"wide cross-bank access");
    /* A full-HD canvas alone crosses 32 banks. Keep it disjoint from the
     * application's extended globals and exercise COW in this larger region. */
    heap=malloc(2500003UL);
    require(heap!=NULL,"large heap allocation");
    memset(heap,0x5a,2500003UL);
    heap[2500002UL]=0xbeef;
    pid=fork();require(pid>=0,"large-heap fork");
    if(!pid){heap[2500002UL]=0x1234;_exit(heap[0]==0x5a?0:1);}
    require(waitpid(pid,&status,0)==pid&&status==0&&heap[2500002UL]==0xbeef,"large-heap COW");
    require(*tail==21 && *edge==19,"heap does not overwrite globals");
    free(heap);
    fd=open("/tmp/global-span",O_RDWR|O_CREAT|O_TRUNC,0600);
    require(fd>=0,"file setup");
    address.pointer=data;
    crossing=64000UL-address.cells[0];
    if(crossing<200) crossing+=64000UL;
    start=crossing-200;
    for(i=start;i<start+400;i++) data[i]=(unsigned)i;
    require(write(fd,data+start,400)==400,"cross-bank syscall source");
    memset(data+start,0,400);
    require(lseek(fd,0,SEEK_SET)==0 && read(fd,data+start,400)==400,"cross-bank syscall destination");
    for(i=start;i<start+400;i++) require(data[i]==(unsigned)i,"cross-bank file contents");
    close(fd);
    pid=fork(); require(pid>=0,"fork");
    if (!pid) {
        data[0]=101; *tail=102; wide[35002L]=103;
        _exit(data[0]==101 && *tail==102 && wide[35002L]==103 ? 23 : 1);
    }
    require(waitpid(pid,&status,0)==pid && status==(23<<8),"child global writes");
    require(data[0]==17 && *tail==21 && wide[35002L]==0x12340000UL+35002UL,"private globals after fork");
    data[0]=200; *tail=201;
    { char *args[]={argv[0],"again",NULL};
      require(execv("/not-a-program",args)<0 && data[0]==200 && *tail==201,"failed exec preserves globals");
      printf("GLOBAL_BANKS_OK %u\n",count);
      execv(argv[0],args);
    }
    require(0,"exec reset");
    return 1;
}
