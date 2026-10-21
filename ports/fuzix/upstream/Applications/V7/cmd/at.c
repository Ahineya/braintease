/* UNIX V7 source code: see /COPYRIGHT or www.tuhs.org for details. */
/* Riplix adaptation: rewritten queue publisher and time parser. No setuid;
 * private per-owner spool, atomic publication, quoted environment/cwd, list/rm. */
#include "../../../../ripple/schedule.h"
extern char **environ;

static int env_name(const char *s, const char *end) {
    if (s==end || (!isalpha((unsigned char)*s) && *s!='_')) return 0;
    while (++s<end) if (!isalnum((unsigned char)*s) && *s!='_') return 0;
    return 1;
}
static int list_remove(const char *spool, const char *id) {
    char *jobs=sched_path(spool,"jobs"), *running=sched_path(spool,"running");
    DIR *dir; struct dirent *de; int failed=0;
    if (!jobs || !running) { free(jobs); free(running); return 1; }
    if (id) {
        char *p, *r; struct stat st;
        if (!sched_id(id)) { fprintf(stderr,"at: invalid job id\n"); failed=1; goto done; }
        p=sched_path(jobs,id); r=sched_path(running,id);
        if (!p || !r) { free(p); free(r); failed=1; goto done; }
        if (!lstat(r,&st)) { fprintf(stderr,"at: job already claimed\n"); failed=1; }
        else if (unlink(p)<0) { perror(p); failed=1; }
        free(p); free(r); goto done;
    }
    dir=opendir(jobs); if (!dir) { perror(jobs); failed=1; goto done; }
    errno=0;
    while ((de=readdir(dir))!=NULL) {
        char *path; time_t due; struct tm *t;
        if (!sched_id(de->d_name)) continue;
        path=sched_path(jobs,de->d_name);
        if (!path) { failed=1; break; }
        if (sched_job(path,de->d_name,&due)<0) { perror(path); failed=1; }
        else if ((t=localtime(&due))!=NULL)
            printf("%s %04d-%02d-%02d %02d:%02d:%02d\n",de->d_name,t->tm_year+1900,t->tm_mon+1,t->tm_mday,t->tm_hour,t->tm_min,t->tm_sec);
        free(path); errno=0;
    }
    if (errno) { perror(jobs); failed=1; }
    closedir(dir);
done:
    free(jobs); free(running); return failed || ferror(stdout);
}
static int when(int argc, char **argv, time_t *due) {
    time_t now=time(NULL); unsigned long count,scale; struct tm local,*t;
    char stamp[32]; unsigned long hour,minute; const char *colon; char h[3],m[3];
    if (now==(time_t)-1 || !argc) return 0;
    if (!strcmp(argv[0],"now")) {
        if (argc==1) { *due=now; return 1; }
        if (argc!=4 || strcmp(argv[1],"+") || !sched_number(argv[2],SCHED_EPOCH_MAX,&count)) return 0;
        if (!strcmp(argv[3],"minute") || !strcmp(argv[3],"minutes")) scale=60;
        else if (!strcmp(argv[3],"hour") || !strcmp(argv[3],"hours")) scale=3600;
        else if (!strcmp(argv[3],"day") || !strcmp(argv[3],"days")) scale=86400;
        else return 0;
        if (count>(SCHED_EPOCH_MAX-(unsigned long)now)/scale) return 0;
        *due=now+count*scale; return 1;
    }
    colon=strchr(argv[0],':');
    if (!colon || colon-argv[0]<1 || colon-argv[0]>2 || strlen(colon+1)!=2 || argc>2) return 0;
    memcpy(h,argv[0],colon-argv[0]); h[colon-argv[0]]=0; strcpy(m,colon+1);
    if (!sched_number(h,23,&hour) || !sched_number(m,59,&minute)) return 0;
    t=localtime(&now); if (!t) return 0; local=*t;
    if (argc==2) {
        const char *d=argv[1]; unsigned i;
        if (strlen(d)!=10 || d[4]!='-' || d[7]!='-') return 0;
        for (i=0;i<10;i++) if (i!=4 && i!=7 && !isdigit((unsigned char)d[i])) return 0;
        sprintf(stamp,"%.4s%.2s%.2s%02lu%02lu",d,d+5,d+8,hour,minute);
        return sched_time(stamp,due) && *due>=now;
    }
    local.tm_hour=(int)hour; local.tm_min=(int)minute; local.tm_sec=0; local.tm_isdst=-1;
    if (!sched_local(&local,due)) return 0;
    if (*due<=now) { local.tm_mday++; local.tm_isdst=-1; if (!sched_local(&local,due)) return 0; }
    return *due!=(time_t)-1 && (unsigned long)*due<=SCHED_EPOCH_MAX;
}
static int publish(const char *spool, const char *input, time_t due) {
    char *jobs=sched_path(spool,"jobs"), *temp=NULL, *target=NULL;
    char name[32], id[32], cwd[256]; FILE *f=NULL,*in=stdin; unsigned serial;
    int fd=-1,c,failed=1; mode_t mask; char **env;
    if (!jobs || !getcwd(cwd,sizeof(cwd))) { perror("at: directory"); goto done; }
    if (input && !(in=fopen(input,"r"))) { perror(input); goto done; }
    for (serial=0;serial<65535U;serial++) {
        sprintf(name,".t%05u-%05u",(unsigned)getpid(),serial);
        temp=sched_path(jobs,name); if (!temp) goto done;
        fd=open(temp,O_WRONLY|O_CREAT|O_EXCL,0600);
        if (fd>=0) break;
        free(temp); temp=NULL; if (errno!=EEXIST) { perror(jobs); goto done; }
    }
    if (fd<0) goto done;
    f=fdopen(fd,"w"); if (!f) { close(fd); goto done; }
    if (chmod(temp,0600)<0) { perror(temp); goto done; }
    fprintf(f,"# riplix-at 1 %lu\n",(unsigned long)due);
    mask=umask(077); umask(mask); fprintf(f,"umask %03o\n",(unsigned)mask);
    for (env=environ;env && *env;env++) {
        const char *equal=strchr(*env,'=');
        if (!equal || !env_name(*env,equal)) continue;
        /* Do not let a captured parser setting alter the generated prologue. */
        if ((equal-*env==3 && !strncmp(*env,"IFS",3)) ||
            (equal-*env==3 && !strncmp(*env,"ENV",3)) ||
            (equal-*env==8 && !strncmp(*env,"BASH_ENV",8))) continue;
        fwrite(*env,1,(size_t)(equal-*env),f); fputc('=',f); sched_quote(f,equal+1);
        fputs("; export ",f); fwrite(*env,1,(size_t)(equal-*env),f); fputc('\n',f);
    }
    fputs("cd ",f); sched_quote(f,cwd); fputs(" || exit 1\n",f);
    while ((c=fgetc(in))!=EOF) fputc(c,f);
    fputc('\n',f);
    if (ferror(in) || ferror(f) || fflush(f)<0) { perror("at: write"); goto done; }
#ifdef RIPPLE_MMU
    /* This kernel's sync interface flushes all files, rather than one fd. */
    sync();
#else
    if (fsync(fileno(f))<0) { perror("at: sync"); goto done; }
#endif
    if (fclose(f)<0) { f=NULL; goto done; } f=NULL;
    for (serial=0;serial<65535U;serial++) {
        sprintf(id,"j%010lu-%05u-%05u",(unsigned long)due,(unsigned)getpid(),serial);
        target=sched_path(jobs,id); if (!target) goto done;
        if (link(temp,target)==0) { failed=0; break; }
        free(target); target=NULL; if (errno!=EEXIST) { perror("at: publish"); goto done; }
    }
    if (!failed) printf("job %s\n",id);
done:
    if (f) fclose(f);
    if (temp) unlink(temp);
    if (in && in!=stdin) fclose(in);
    free(target); free(temp); free(jobs); return failed || ferror(stdout);
}
int main(int argc, char **argv) {
    const char *spool=AT_SPOOL,*input=NULL,*stamp=NULL,*remove=NULL; int i,list=0; time_t due;
    for (i=1;i<argc;i++) {
        if (!strcmp(argv[i],"--")) { i++; break; }
        if (!strcmp(argv[i],"-l")) list=1;
        else if ((!strcmp(argv[i],"-s") || !strcmp(argv[i],"-f") || !strcmp(argv[i],"-t") || !strcmp(argv[i],"-r")) && i+1<argc) {
            char option=argv[i][1]; const char *value=argv[++i];
            if (option=='s') spool=value; else if (option=='f') input=value;
            else if (option=='t') stamp=value; else remove=value;
        } else break;
    }
    if ((list || remove) && (i!=argc || input || stamp || (list && remove))) goto usage;
    if (!list && !remove) {
        if (stamp ? i!=argc || !sched_time(stamp,&due) || due<time(NULL) : !when(argc-i,argv+i,&due)) goto usage;
    }
    if (sched_spool(spool,1)<0) { perror(spool); return 1; }
    if (list || remove) return list_remove(spool,remove);
    return publish(spool,input,due);
usage:
    fprintf(stderr,"usage: at [-s SPOOL] [-f FILE] {now [+ N minutes|hours|days] | HH:MM [YYYY-MM-DD] | -t YYYYMMDDhhmm[.ss]}\n       at [-s SPOOL] {-l | -r JOB}\n"); return 2;
}
