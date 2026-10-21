#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/wait.h>
#include <signal.h>
static void goodbye(void) { puts("SDK_ATEXIT_OK"); }
static volatile int delivered;
static void caught(int n) { delivered=n; }
int main(int argc, char **argv, char **envp)
{
    char text[128], readback[128];
    unsigned char *p = calloc(70000UL, 1);
    FILE *f;
    unsigned long i;
    int channel[2], status;
    pid_t child;
    if (argc == 2 && !strcmp(argv[1], "--child")) {
        if (!getenv("PATH")) return 11;
        puts("SDK_EXEC_OK");
        return 12;
    }
    if (argc != 3 || strcmp(argv[1], "hello") || strcmp(argv[2], "world") || argv[3]) return 1;
    if (!envp || !getenv("PATH")) return 2;
    if (!p || p[69999]) return 3;
    for (i=0;i<70000UL;i++) p[i]=(unsigned)(i^0xabcdUL);
    p=realloc(p,71000UL);
    if (!p || p[0]!=0xabcdU || p[69999]!=(unsigned)(69999UL^0xabcdUL)) return 4;
    free(p);
    snprintf(text,sizeof(text),"%d %lu %llu %s",-7,305419896UL,18364758544493064720ULL,"ok");
    if (strcmp(text,"-7 305419896 18364758544493064720 ok")) return 5;
    f=fopen("/tmp/sdk-data","w");
    if (!f || fprintf(f,"%s\n",text)<0 || fclose(f)) return 6;
    f=fopen("/tmp/sdk-data","r");
    if (!f || !fgets(readback,sizeof(readback),f) || fclose(f)) return 7;
    strcat(text,"\n");
    if (strcmp(text,readback) || unlink("/tmp/sdk-data")) return 8;
    errno=0;
    if (fopen("/tmp/not-present-sdk","r") || errno!=ENOENT) return 9;
    if (pipe(channel) || fflush(NULL)) return 13;
    child=fork();
    if (child<0) return 14;
    if (!child) {
        char *args[3];
        close(channel[0]);
        if (write(channel[1],"child",5)!=5) _exit(15);
        close(channel[1]);
        args[0]=argv[0];args[1]="--child";args[2]=NULL;
        execvp(args[0],args);
        _exit(16);
    }
    close(channel[1]);
    memset(readback,0,sizeof(readback));
    if (read(channel[0],readback,5)!=5 || strcmp(readback,"child")) return 17;
    close(channel[0]);
    if (waitpid(child,&status,0)!=child || !WIFEXITED(status) || WEXITSTATUS(status)!=12) return 18;
    if (signal(SIGUSR1,caught)==SIG_ERR || raise(SIGUSR1) || delivered!=SIGUSR1) return 19;
    if (atexit(goodbye)) return 10;
    printf("SDK_MAIN_OK %s %s\n",argv[1],argv[2]);
    return 0;
}
