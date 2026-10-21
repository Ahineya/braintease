/* UNIX V7 source code: see /COPYRIGHT or www.tuhs.org for details. */
/* Riplix adaptation: rewritten parser/runner; supervised foreground operation,
 * local-time matching, bounded input, concurrent children and graceful stop. */
#include "../../../../ripple/schedule.h"

static volatile sig_atomic_t stopping;
static void stop(int sig) { (void)sig; stopping=1; }
static int launch(const char *command, const char *spool) {
    pid_t pid=fork();
    if (pid<0) { perror("cron: fork"); return -1; }
    if (!pid) {
        int fd;
        signal(SIGTERM,SIG_DFL); signal(SIGINT,SIG_DFL); signal(SIGHUP,SIG_DFL);
        fd=open("/dev/null",O_RDONLY);
        if (fd<0 || dup2(fd,0)<0) { perror("cron: stdin"); _exit(126); }
        if (fd>2) close(fd);
        if (chdir("/")<0) _exit(126);
        if (spool) {
            char *args[]={"atrun","-s",(char *)spool,NULL}; execv("/bin/atrun",args);
        } else {
            char *args[]={"sh","-c",(char *)command,NULL}; execv("/bin/sh",args);
        }
        perror("cron: exec"); _exit(127);
    }
    return 0;
}
static int reap(int wait_all) {
    int status, failed=0; pid_t pid;
    for (;;) {
        pid=waitpid(-1,&status,wait_all?0:WNOHANG);
        if (pid<0 && errno==EINTR) continue;
        if (pid<=0) break;
        if (!WIFEXITED(status) || WEXITSTATUS(status)) {
            fprintf(stderr,"cron: job %u failed (wait status %d)\n",(unsigned)pid,status); failed=1;
        }
    }
    return failed;
}
int main(int argc, char **argv) {
    const char *config="/etc/crontab", *spool=AT_SPOOL;
    int i, once=0, check=0, explicit_time=0, failed=0; unsigned long epoch=0, last=0;
    int have_last=0; time_t now;
    for (i=1;i<argc;i++) {
        if ((!strcmp(argv[i],"-c") || !strcmp(argv[i],"-s")) && i+1<argc) {
            if (argv[i][1]=='c') config=argv[++i]; else spool=argv[++i];
        } else if (!strcmp(argv[i],"--once")) once=1;
        else if (!strcmp(argv[i],"-t")) check=1;
        else if (!strcmp(argv[i],"-f")) { /* Always foreground. */ }
        else if (!strcmp(argv[i],"--no-at")) spool=NULL;
        else if (!strcmp(argv[i],"--time") && i+1<argc) {
            if (!sched_number(argv[++i],SCHED_EPOCH_MAX,&epoch)) goto usage;
            explicit_time=1;
        } else goto usage;
    }
    if (explicit_time && !once) goto usage;
    if (!getenv("PATH") && putenv("PATH=/bin:/usr/bin")) { perror("cron: environment"); return 1; }
    signal(SIGTERM,stop); signal(SIGINT,stop); signal(SIGHUP,SIG_IGN);
    do {
        struct cron_entry *head=NULL, *e; struct tm *tm;
        now=explicit_time?(time_t)epoch:time(NULL);
        if (now==(time_t)-1) { perror("cron: clock"); return 1; }
        if (check || !have_last || (unsigned long)now/60>last) {
            last=(unsigned long)now/60; have_last=1;
            if (sched_cron_load(config,&head)<0) { perror(config); failed=1; }
            else if (!check) {
                tm=localtime(&now);
                if (!tm) { sched_cron_free(head); return 1; }
                for (e=head;e && !stopping;e=e->next)
                    if (sched_cron_match(e,tm) && launch(e->command,NULL)<0) failed=1;
            }
            sched_cron_free(head);
            /* The queue runner is independent of malformed/missing crontabs. */
            if (!check && spool && !stopping && launch(NULL,spool)<0) failed=1;
        }
        if (check) return failed;
        if (reap(once)) failed=1;
        if (!once && !stopping) sleep(1);
    } while (!once && !stopping);
    /* svcd signals the entire service group, including jobs, on shutdown.
     * A direct TERM stops new dispatch; outstanding children retain the group. */
    return once?failed:0;
usage:
    fprintf(stderr,"usage: cron [-f] [-c FILE] [-s SPOOL] [--no-at] [-t | --once [--time EPOCH]]\n");
    return 2;
}
