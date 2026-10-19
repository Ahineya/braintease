/* SPDX-License-Identifier: GPL-2.0-only */
/* Minimal separate directory-listing executable, without sorting/long format. */
#include "user.h"
extern void perror(const char *);
static int list(const char *path) {
    struct dirent entry;
    long n;
    int fd=open(path,0);
    if (fd<0) { perror(path); return 1; }
    while ((n=call(24,fd,(long)&entry,sizeof(entry),0))>0) {
        if (entry.ino) {
            unsigned len=0;
            while (len<30 && entry.name[len]) ++len;
            if (write(1,entry.name,len)!=len || write(1,"\n",1)!=1) {
                perror("ls"); close(fd); return 1;
            }
        }
    }
    if (n<0) perror(path);
    close(fd); return n<0;
}
int main(int argc,char **argv) {
    int i,result=0;
    if (argc<2) return list(".");
    for (i=1;i<argc;++i) result|=list(argv[i]);
    return result;
}
