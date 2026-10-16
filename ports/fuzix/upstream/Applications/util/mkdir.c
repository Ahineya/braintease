/* Ripple adaptation of FUZIX util/mkdir.c: bounded, iterative -p handling. */
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <errno.h>
static int make_one(const char *p,mode_t mode,int parents) {
    struct stat st;
    if(mkdir(p,mode)==0) return 0;
    if(parents && errno==EEXIST && stat(p,&st)==0 && S_ISDIR(st.st_mode)) return 0;
    perror(p);return 1;
}
int main(int argc,char **argv) {
    int c,parents=0,failed=0,i;mode_t mode=0777;
    char path[512],*p,*end;
    while((c=getopt(argc,argv,"pm:"))!=-1) {
        if(c=='p') parents=1;
        else if(c=='m') {unsigned long n=strtoul(optarg,&end,8);if(*end||n>07777) return 2;mode=n;}
        else {fputs("usage: mkdir [-p] [-m mode] directory...\n",stderr);return 2;}
    }
    if(optind==argc) {fputs("mkdir: missing directory\n",stderr);return 2;}
    for(i=optind;i<argc;i++) {
        if(strlcpy(path,argv[i],sizeof(path))>=sizeof(path)) {errno=ENAMETOOLONG;perror(argv[i]);failed=1;continue;}
        if(parents) for(p=path+1;*path && *p;p++) if(*p=='/' && p[-1]!='/') {
            *p=0;failed|=make_one(path,0777,1);*p='/';
        }
        failed|=make_one(path,mode,parents);
    }
    return failed;
}
