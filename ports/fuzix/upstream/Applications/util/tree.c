/* Derived from FUZIX Applications/util/tree.c (pinned upstream revision).
 * Riplix adaptation: sorted physical walk, symlink labels, depth/hidden filters,
 * checked allocation/path/I/O errors. Never traverses symbolic links. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <getopt.h>
#include <sys/stat.h>
#include <dirent.h>
#include <errno.h>

static int hidden,dirs,failed;
static unsigned depth_limit=65535U;
static unsigned long directory_count,file_count;
struct item { char *name; mode_t mode; };
static int compare(const void *a,const void *b) {
    return strcmp(((const struct item *)a)->name,((const struct item *)b)->name);
}
static char *path_join(const char *a,const char *b) {
    size_t n=strlen(a),m=strlen(b); char *p;
    if (n+m+2>256) { errno=ENAMETOOLONG; return NULL; }
    p=malloc(n+m+2);
    if (p) { memcpy(p,a,n); p[n]='/'; memcpy(p+n+1,b,m+1); }
    return p;
}
static void error(const char *path) { fprintf(stderr,"tree: "); perror(path); failed=1; }
static void label(const char *path,const char *name,mode_t mode) {
    fputs(name,stdout);
    if (S_ISLNK(mode)) {
        char target[256]; ssize_t n=readlink(path,target,sizeof(target)-1);
        if (n<0) error(path);
        else { target[n]=0; printf(" -> %s",target); }
    }
    putchar('\n');
}
static void walk(const char *path,unsigned level,const char *prefix) {
    DIR *dir; struct dirent *de; struct item *items=NULL;
    size_t count=0,capacity=0,i;
    if (level>=depth_limit) return;
    dir=opendir(path); if (!dir) { error(path); return; }
    for (;;) {
        char *full; struct stat st;
        errno=0; de=readdir(dir);
        if (!de) { if (errno) error(path); break; }
        if (!strcmp(de->d_name,".") || !strcmp(de->d_name,"..") || (!hidden && de->d_name[0]=='.')) continue;
        full=path_join(path,de->d_name);
        if (!full) { error(path); continue; }
        if (lstat(full,&st)<0) { error(full); free(full); continue; }
        free(full);
        if (dirs && !S_ISDIR(st.st_mode)) continue;
        if (count==capacity) {
            size_t next=capacity?capacity*2:16; struct item *p;
            if (next<capacity || next>((size_t)-1)/sizeof(*p)) { errno=ENOMEM; error(path); break; }
            p=realloc(items,next*sizeof(*p)); if (!p) { error(path); break; }
            items=p; capacity=next;
        }
        items[count].name=strdup(de->d_name);
        if (!items[count].name) { error(path); break; }
        items[count++].mode=st.st_mode;
    }
    closedir(dir); qsort(items,count,sizeof(*items),compare);
    for (i=0;i<count;i++) {
        char *full=path_join(path,items[i].name);
        if (full) {
            printf("%s%s",prefix,i+1==count?"`-- ":"|-- ");
            label(full,items[i].name,items[i].mode);
            if (S_ISDIR(items[i].mode)) {
                size_t n=strlen(prefix); char *next=malloc(n+5);
                directory_count++;
                if (!next) error(full);
                else { memcpy(next,prefix,n); strcpy(next+n,i+1==count?"    ":"|   "); walk(full,level+1,next); free(next); }
            } else file_count++;
            free(full);
        } else error(path);
        free(items[i].name);
    }
    free(items);
}
int main(int argc,char **argv) {
    int c,i; unsigned long n; char *end;
    while ((c=getopt(argc,argv,"adL:"))!=-1) {
        if (c=='a') hidden=1;
        else if (c=='d') dirs=1;
        else if (c=='L') {
            errno=0; n=strtoul(optarg,&end,10);
            if (errno || !*optarg || *end || optarg[0]=='-' || !n || n>65535UL) goto usage;
            depth_limit=(unsigned)n;
        } else goto usage;
    }
    for (i=optind;i<argc || (i==optind && i==argc);i++) {
        const char *path=i<argc?argv[i]:"."; struct stat st;
        if (lstat(path,&st)<0) error(path);
        else { label(path,path,st.st_mode); if (S_ISDIR(st.st_mode)) walk(path,0,""); }
        if (i==argc) break;
    }
    if (dirs) printf("\n%lu directories\n",directory_count);
    else printf("\n%lu directories, %lu files\n",directory_count,file_count);
    return failed || ferror(stdout);
usage:
    fprintf(stderr,"usage: tree [-ad] [-L LEVEL] [--] [DIRECTORY ...]\n"); return 2;
}
