/* UNIX V7 source code: see /COPYRIGHT or www.tuhs.org for details. */
/* Riplix adaptation: atomic hard-link claim before unlinking pending jobs;
 * concurrent invocations cannot dispatch the same queued file twice. */
#include "../../../../ripple/schedule.h"

static int run(const char *jobs,const char *running,const char *output,const char *id,time_t now) {
    char *pending=sched_path(jobs,id), *claimed=sched_path(running,id), *log=sched_path(output,id);
    time_t due; pid_t pid; int result=0;
    if (!pending || !claimed || !log) { result=1; goto done; }
    if (sched_job(pending,id,&due)<0) { if (errno!=ENOENT) { perror(pending); result=1; } goto done; }
    if (due>now) goto done;
    if (link(pending,claimed)<0) {
        if (errno!=EEXIST && errno!=ENOENT) { perror(claimed); result=1; }
        goto done;
    }
    if (unlink(pending)<0) { perror(pending); result=1; goto done; }
    pid=fork();
    if (pid<0) { perror("atrun: fork (claimed job retained)"); result=1; goto done; }
    if (!pid) {
        pid_t child; int status,fd,in;
        fd=open(log,O_WRONLY|O_CREAT|O_EXCL,0600);
        if (fd<0) { perror(log); _exit(1); }
        in=open("/dev/null",O_RDONLY);
        if (in<0 || dup2(in,0)<0 || dup2(fd,1)<0 || dup2(fd,2)<0) _exit(1);
        if (in>2) close(in);
        if (fd>2) close(fd);
        child=fork();
        if (child<0) { perror("atrun: shell fork"); _exit(1); }
        if (!child) {
            char *args[]={"sh",claimed,NULL};
            execv("/bin/sh",args); perror("atrun: shell"); _exit(127);
        }
        while (waitpid(child,&status,0)<0) if (errno!=EINTR) _exit(1);
        if (!WIFEXITED(status) || WEXITSTATUS(status)) fprintf(stderr,"atrun: job failed (wait status %d)\n",status);
        if (unlink(claimed)<0) _exit(1);
        _exit(!WIFEXITED(status) || WEXITSTATUS(status) ? 1:0);
    }
done:
    free(pending); free(claimed); free(log); return result;
}
int main(int argc, char **argv) {
    const char *spool=AT_SPOOL; char *jobs,*running,*output;
    DIR *dir; struct dirent *de; time_t now; int failed=0,status; pid_t child;
    if (argc==3 && !strcmp(argv[1],"-s")) spool=argv[2];
    else if (argc!=1) { fprintf(stderr,"usage: atrun [-s SPOOL]\n"); return 2; }
    if (sched_spool(spool,1)<0) { perror(spool); return 1; }
    jobs=sched_path(spool,"jobs"); running=sched_path(spool,"running"); output=sched_path(spool,"output");
    if (!jobs || !running || !output) { free(jobs); free(running); free(output); return 1; }
    now=time(NULL); if (now==(time_t)-1) { free(jobs); free(running); free(output); return 1; }
    dir=opendir(jobs);
    if (!dir) { perror(jobs); free(jobs); free(running); free(output); return 1; }
    for (;;) {
        errno=0; de=readdir(dir);
        if (!de) { if (errno) { perror(jobs); failed=1; } break; }
        if (sched_id(de->d_name) && run(jobs,running,output,de->d_name,now)) failed=1;
    }
    closedir(dir); free(jobs); free(running); free(output);
    for (;;) {
        child=waitpid(-1,&status,0);
        if (child<0) { if (errno==EINTR) continue; break; }
        if (!WIFEXITED(status) || WEXITSTATUS(status)) failed=1;
    }
    return failed;
}
