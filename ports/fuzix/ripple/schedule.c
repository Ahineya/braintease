/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Shared queue validation and calendar parsing for the adapted V7 schedulers. */
#include "schedule.h"

int sched_number(const char *s, unsigned long limit, unsigned long *out) {
    unsigned long n=0;
    if (!*s) return 0;
    while (*s) {
        unsigned d=(unsigned)(*s++-'0');
        if (d>9 || n>limit/10 || (n==limit/10 && d>limit%10)) return 0;
        n=n*10+d;
    }
    *out=n; return 1;
}
char *sched_path(const char *dir, const char *name) {
    size_t a=strlen(dir), b=strlen(name);
    char *p;
    if (a>250 || b>250 || a+b+2>256) { errno=ENAMETOOLONG; return NULL; }
    p=malloc(a+b+2);
    if (p) { memcpy(p,dir,a); p[a]='/'; memcpy(p+a+1,name,b+1); }
    return p;
}
int sched_directory(const char *path, int create) {
    struct stat st;
    if (create) {
        if (mkdir(path,0700)==0) { if (chmod(path,0700)<0) return -1; }
        else if (errno!=EEXIST) return -1;
    }
    if (lstat(path,&st)<0) return -1;
    if (!S_ISDIR(st.st_mode) || st.st_uid!=geteuid() || (st.st_mode&0077)) {
        errno=EACCES; return -1;
    }
    return 0;
}
int sched_spool(const char *path, int create) {
    static const char *names[]={"jobs","running","output"};
    unsigned i;
    if (*path!='/') { errno=EINVAL; return -1; }
    /* Reserve room for /running/ plus a 23-character job identifier. */
    if (strlen(path)>223) { errno=ENAMETOOLONG; return -1; }
    if (sched_directory(path,create)<0) return -1;
    for (i=0;i<3;i++) {
        char *p=sched_path(path,names[i]); int result;
        if (!p) return -1;
        result=sched_directory(p,create); free(p);
        if (result<0) return -1;
    }
    return 0;
}
int sched_quote(FILE *f, const char *s) {
    if (fputc('\'',f)==EOF) return -1;
    while (*s) {
        if (*s=='\'') { if (fputs("'\\''",f)==EOF) return -1; }
        else if (fputc(*s,f)==EOF) return -1;
        s++;
    }
    return fputc('\'',f)==EOF ? -1 : 0;
}
/* Queue/config inputs must be owned by this scheduler's effective user. */
int sched_file(const char *path, struct stat *st) {
    if (lstat(path,st)<0) return -1;
    if (!S_ISREG(st->st_mode) || st->st_uid!=geteuid() || (st->st_mode&0022)) {
        errno=EACCES; return -1;
    }
    return 0;
}
int sched_line(FILE *f, char *line, unsigned size) {
    unsigned n=0; int c, bad=0;
    while ((c=fgetc(f))!=EOF && c!='\n') {
        if (!c || n+1>=size) bad=1;
        else line[n++]=(char)c;
    }
    if (ferror(f)) return -1;
    if (!n && c==EOF && !bad) return 0;
    line[n]=0;
    if (bad) { errno=E2BIG; return -1; }
    return 1;
}
int sched_id(const char *s) {
    unsigned i;
    if (strlen(s)!=23 || s[0]!='j' || s[11]!='-' || s[17]!='-') return 0;
    for (i=1;i<23;i++) if (i!=11 && i!=17 && !isdigit((unsigned char)s[i])) return 0;
    return 1;
}
int sched_job(const char *path, const char *id, time_t *due) {
    struct stat before, after; FILE *f; char line[96], epoch[11];
    unsigned long n; int good;
    if (!sched_id(id) || sched_file(path,&before)<0 || (before.st_mode&0077)) return -1;
    f=fopen(path,"r"); if (!f) return -1;
    good=fstat(fileno(f),&after)==0 && before.st_dev==after.st_dev && before.st_ino==after.st_ino;
    memcpy(epoch,id+1,10); epoch[10]=0;
    good=good && sched_number(epoch,SCHED_EPOCH_MAX,&n);
    if (good) {
        char expected[96];
        sprintf(expected,"# riplix-at 1 %lu",n);
        good=sched_line(f,line,sizeof(line))==1 && !strcmp(expected,line);
    }
    fclose(f);
    if (!good) { errno=EINVAL; return -1; }
    *due=(time_t)n; return 0;
}

static int field_number(const char **p, int high, int *out) {
    unsigned n=0;
    if (!isdigit((unsigned char)**p)) return 0;
    do { n=n*10+(unsigned)(*(*p)++-'0'); if (n>(unsigned)high) return 0; }
    while (isdigit((unsigned char)**p));
    *out=(int)n; return 1;
}
static int field(const char **p, int low, int high, unsigned char *mask) {
    for (;;) {
        int first, last, step=1, v;
        if (**p=='*') { (*p)++; first=low; last=high; }
        else {
            if (!field_number(p,high,&first) || first<low) return 0;
            last=first;
            if (**p=='-') { (*p)++; if (!field_number(p,high,&last) || last<first) return 0; }
        }
        if (**p=='/') { (*p)++; if (!field_number(p,high+1,&step) || !step) return 0; }
        for (v=first;v<=last;v+=step) mask[v]=1;
        if (**p!=',') break;
        (*p)++;
    }
    if (**p!=' ' && **p!='\t') return 0;
    while (**p==' ' || **p=='\t') (*p)++;
    return 1;
}
void sched_cron_free(struct cron_entry *p) {
    while (p) { struct cron_entry *next=p->next; free(p->command); free(p); p=next; }
}
int sched_cron_load(const char *path, struct cron_entry **head) {
    FILE *f; struct stat st,opened; struct cron_entry **tail=head;
    char line[SCHED_LINE_MAX+1]; unsigned number=0; int result;
    *head=NULL;
    if (sched_file(path,&st)<0 || !(f=fopen(path,"r"))) return -1;
    if (fstat(fileno(f),&opened)<0 || st.st_dev!=opened.st_dev || st.st_ino!=opened.st_ino) {
        fclose(f); errno=EACCES; return -1;
    }
    while ((result=sched_line(f,line,sizeof(line)))>0) {
        const char *p=line; struct cron_entry *entry; int i;
        static const int low[]={0,0,1,1,0}, high[]={59,23,31,12,7};
        number++;
        while (*p==' ' || *p=='\t') p++;
        if (!*p || *p=='#') continue;
        entry=calloc(1,sizeof(*entry)); if (!entry) { result=-1; break; }
        for (i=0;i<5;i++) {
            if (i==2) entry->dom_star=*p=='*';
            if (i==4) entry->dow_star=*p=='*';
            if (!field(&p,low[i],high[i],entry->fields[i])) break;
        }
        if (i!=5 || !*p) {
            fprintf(stderr,"cron: %s:%u: invalid schedule\n",path,number);
            free(entry); errno=EINVAL; result=-1; break;
        }
        /* Unlike old V7 cron, '%' is ordinary shell text, not implicit stdin. */
        entry->fields[4][0]|=entry->fields[4][7];
        entry->command=strdup(p);
        if (!entry->command) { free(entry); result=-1; break; }
        *tail=entry; tail=&entry->next;
    }
    fclose(f);
    if (result<0) { sched_cron_free(*head); *head=NULL; return -1; }
    return 0;
}
int sched_cron_match(const struct cron_entry *e, const struct tm *t) {
    int dom=e->fields[2][t->tm_mday], dow=e->fields[4][t->tm_wday];
    return e->fields[0][t->tm_min] && e->fields[1][t->tm_hour] && e->fields[3][t->tm_mon+1] &&
        ((e->dom_star || e->dow_star) ? dom && dow : dom || dow);
}
/* Convert civil fields without relying on the absent guest mktime symbol.
 * localtime supplies the configured TZ/DST offset. Correction converges for
 * representable local times; gaps are rejected instead of silently normalized. */
static int civil(const struct tm *t, unsigned long *out) {
    static const unsigned days[]={31,28,31,30,31,30,31,31,30,31,30,31};
    int year=t->tm_year+1900,y,m; unsigned long n=0,seconds;
    if (year<1970 || year>2106 || t->tm_mon<0 || t->tm_mon>11 || t->tm_mday<1 ||
        t->tm_mday>32 || t->tm_hour<0 || t->tm_hour>23 || t->tm_min<0 || t->tm_min>59 || t->tm_sec<0 || t->tm_sec>59) return 0;
    for (y=1970;y<year;y++) n+=365+(y%4==0 && (y%100!=0 || y%400==0));
    for (m=0;m<t->tm_mon;m++) n+=days[m]+(m==1 && year%4==0 && (year%100!=0 || year%400==0));
    n+=(unsigned)(t->tm_mday-1);
    seconds=(unsigned long)t->tm_hour*3600+(unsigned long)t->tm_min*60+t->tm_sec;
    if (n>(SCHED_EPOCH_MAX-seconds)/86400) return 0;
    *out=n*86400+seconds; return 1;
}
int sched_local(struct tm *t, time_t *out) {
    unsigned long wanted,observed,candidate; unsigned i;
    if (!civil(t,&wanted)) return 0;
    candidate=wanted;
    for (i=0;i<6;i++) {
        time_t epoch=(time_t)candidate; struct tm local,*p=localtime(&epoch);
        long correction;
        if (!p) return 0;
        local=*p;
        if (!civil(&local,&observed)) return 0;
        if (wanted==observed) { *t=local; *out=epoch; return 1; }
        /* The difference is a timezone offset, so fits signed 32-bit long. */
        correction=wanted>=observed?(long)(wanted-observed):-(long)(observed-wanted);
        if (correction<0) { if (candidate<(unsigned long)-correction) return 0; candidate-=(unsigned long)-correction; }
        else { if (candidate>SCHED_EPOCH_MAX-(unsigned long)correction) return 0; candidate+=(unsigned long)correction; }
    }
    return 0;
}
int sched_time(const char *stamp, time_t *out) {
    struct tm t, original, *round; int i, n[6]={0,0,0,0,0,0};
    static const int widths[]={4,2,2,2,2,2}; const char *p=stamp;
    if (strlen(stamp)!=12 && strlen(stamp)!=15) return 0;
    for (i=0;i<6;i++) {
        int j;
        if (i==5) { if (!*p) break; if (*p++!='.') return 0; }
        for (j=0;j<widths[i];j++) { if (!isdigit((unsigned char)*p)) return 0; n[i]=n[i]*10+*p++-'0'; }
    }
    if (*p || n[0]<1970 || n[0]>2106 || n[1]<1 || n[1]>12 || n[2]<1 || n[2]>31 ||
        n[3]>23 || n[4]>59 || n[5]>59) return 0;
    memset(&t,0,sizeof(t)); t.tm_year=n[0]-1900; t.tm_mon=n[1]-1; t.tm_mday=n[2];
    t.tm_hour=n[3]; t.tm_min=n[4]; t.tm_sec=n[5]; t.tm_isdst=-1; original=t;
    if (!sched_local(&t,out) || !(round=localtime(out))) return 0;
    return round->tm_year==original.tm_year && round->tm_mon==original.tm_mon && round->tm_mday==original.tm_mday &&
        round->tm_hour==original.tm_hour && round->tm_min==original.tm_min && round->tm_sec==original.tm_sec;
}
