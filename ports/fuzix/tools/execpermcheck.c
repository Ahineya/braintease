/* Check exec/access permissions independently of the shell's script fallback. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/wait.h>
#define C(x) do { if (!(x)) { printf("EXECPERM_FAIL line %d errno %d: %s\n",__LINE__,errno,#x); return 1; } } while (0)
static struct permission_case { unsigned mode, uid, gid; int user_exec; } cases[] = {
    {0644,0,0,0}, {0000,0,0,0}, {0744,0,0,0}, {0654,0,100,1},
    {0645,0,0,1}, {0645,100,100,0}, {0744,100,100,1}, {0744,0,100,0},
    {0001,0,0,1}, {0010,0,100,1}, {0100,100,100,1}
};
static int check(int unprivileged) {
    unsigned i; char path[64];
    char *argv[]={"permission-script",NULL};
    char *envp[]={NULL};
    for(i=0;i<sizeof(cases)/sizeof(cases[0]);i++) {
        int executable=unprivileged?cases[i].user_exec:!!(cases[i].mode&0111);
        sprintf(path,"/tmp/exec-permissions/file-%u",i);
        if(executable) C(access(path,X_OK)==0);
        else C(access(path,X_OK)==-1 && errno==EACCES);
        C(execve(path,argv,envp)==-1);
        C(errno==(executable?ENOEXEC:EACCES));
    }
    return 0;
}
int main(void) {
    static const char text[]="echo THIS_TEXT_IS_ONLY_INSPECTED_BY_EXECVE\n";
    unsigned i; int fd, pid, status; char path[64];
    C(mkdir("/tmp/exec-permissions",0755)==0);
    for(i=0;i<sizeof(cases)/sizeof(cases[0]);i++) {
        sprintf(path,"/tmp/exec-permissions/file-%u",i);
        fd=open(path,O_CREAT|O_WRONLY,0600);C(fd>=0);
        C(write(fd,text,strlen(text))==(int)strlen(text));C(close(fd)==0);
        C(chown(path,cases[i].uid,cases[i].gid)==0);
        C(chmod(path,cases[i].mode)==0);
    }
    C(check(0)==0);
    C(mkdir("/tmp/exec-permissions/root-search",0000)==0);
    C(access("/tmp/exec-permissions/root-search",X_OK)==0);
    pid=fork();C(pid>=0);
    if(!pid) {
        if(setgid(100)<0 || setuid(100)<0) exit(2);
        exit(check(1));
    }
    C(waitpid(pid,&status,0)==pid && WIFEXITED(status) && WEXITSTATUS(status)==0);
    puts("EXECPERM_OK root/user execute bits, access and execve");return 0;
}
