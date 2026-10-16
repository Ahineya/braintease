/* Ripple adaptation of FUZIX util/rm.c: recursive removal and error status. */
#include <string.h>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>
#include <stdlib.h>
#include <dirent.h>
#include <errno.h>
static int recursive,force,verbose,failed;
static void report(const char *p) {
    if(force && errno==ENOENT) return;
    fprintf(stderr,"rm: %s: %s\n",p,strerror(errno));failed=1;
}
static void remove_path(const char *p) {
    struct stat st;
    if(lstat(p,&st)<0) {report(p);return;}
    if(S_ISDIR(st.st_mode)) {
        DIR *d;struct dirent *e;char child[512];int ecode;
        if(!recursive) {errno=EISDIR;report(p);return;}
        d=opendir(p);if(!d) {report(p);return;}
        for(;;) {
            errno=0;e=readdir(d);ecode=errno;
            if(!e) {if(ecode) report(p);break;}
            if(!strcmp(e->d_name,".") || !strcmp(e->d_name,"..")) continue;
            if(snprintf(child,sizeof(child),"%s/%s",p,e->d_name)>=(int)sizeof(child)) {errno=ENAMETOOLONG;report(p);break;}
            remove_path(child);
        }
        closedir(d);
        if(rmdir(p)<0) {report(p);return;}
    } else if(unlink(p)<0) {report(p);return;}
    if(verbose) puts(p);
}
int main(int argc,char **argv) {
    int i=1,j;char path[512];size_t n;char *base;
    while(i<argc && argv[i][0]=='-' && argv[i][1]) {
        if(!strcmp(argv[i],"--")) {i++;break;}
        for(j=1;argv[i][j];j++) {
            if(argv[i][j]=='r'||argv[i][j]=='R') recursive=1;
            else if(argv[i][j]=='f') force=1;
            else if(argv[i][j]=='v') verbose=1;
            else {fputs("usage: rm [-rfv] [--] file...\n",stderr);return 2;}
        }
        i++;
    }
    if(i==argc && !force) {fputs("usage: rm [-rfv] [--] file...\n",stderr);return 2;}
    for(;i<argc;i++) {
        if(strlcpy(path,argv[i],sizeof(path))>=sizeof(path)) {errno=ENAMETOOLONG;report(argv[i]);continue;}
        n=strlen(path);while(n>1 && path[n-1]=='/') path[--n]=0;
        base=strrchr(path,'/');base=base ? base+1:path;
        if(!strcmp(path,"/") || !strcmp(base,".") || !strcmp(base,"..")) {
            fprintf(stderr,"rm: refusing %s\n",argv[i]);failed=1;continue;
        }
        remove_path(path);
    }
    return failed;
}
