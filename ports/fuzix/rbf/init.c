/* SPDX-License-Identifier: GPL-2.0-only */
/* Disposable-disk adaptation of the original Ripple FUZIX init shell (0c7a90a0).
 * This is a separately loaded userspace program, not a kernel command loop. */
#include "user.h"
static char line[256];
static char *args[24];
static char cwd[256] = "/";
static void failure(void) {
    out("error "); number(ripple_error); out("\n");
}
static unsigned last_status;
extern void perror(const char *);
static void echo(int argc) {
    int fd=1,end=argc,i;
    for (i=1;i<argc;++i) {
        if (!strcmp(args[i],">") || !strcmp(args[i],">>")) {
            if (i!=argc-2) { out("usage: echo text [> or >> file]\n"); return; }
            fd=open(args[i+1],1|256|(!strcmp(args[i],">>")?4:1024),0644);
            if (fd<0) { failure(); return; }
            end=i; break;
        }
    }
    for (i=1;i<end;++i) {
        if ((i>1 && write(fd," ",1)!=1) || write(fd,args[i],strlen(args[i]))!=strlen(args[i])) {
            failure(); break;
        }
    }
    if (write(fd,"\n",1)!=1) failure();
    if (fd!=1) close(fd);
}
static void run(void) {
    char path[128];
    unsigned length=strlen(args[0]);
    int status;
    long pid;
    if (args[0][0]=='/' || args[0][0]=='.') {
        if (length>=sizeof(path)) { out("program path too long\n"); return; }
        memcpy(path,args[0],length+1);
    } else {
        if (length+5>=sizeof(path)) { out("program path too long\n"); return; }
        memcpy(path,"/bin/",5); memcpy(path+5,args[0],length+1);
    }
    pid=call(32,0,0,0,0);
    if (pid<0) { failure(); last_status=1; return; }
    if (!pid) {
        call(23,(long)path,(long)args,0,0);
        perror(path);
        call(0,127,0,0,0);
        for (;;) {}
    }
    if (call(55,pid,(long)&status,0,0)<0) { failure(); last_status=1; }
    else last_status=((unsigned)status&127)?128+((unsigned)status&127):((unsigned)status>>8);
}
static int tokenize(void) {
    unsigned n=0;
    char *s=line;
    while (*s) {
        while (*s==' ' || *s=='\t') ++s;
        if (!*s) break;
        if (n==23) { out("too many arguments\n"); return 0; }
        args[n++]=s;
        while (*s && *s!=' ' && *s!='\t') ++s;
        if (*s) *s++=0;
    }
    args[n]=NULL;
    return n;
}
/* There are no symlinks in this image. Resolve components before chdir so
 * pwd stays canonical and a failed chdir leaves the old location intact. */
static void cd(const char *path) {
    const char *original=path;
    char dest[256];
    unsigned len=0,n,start;
    if (*path!='/') { len=strlen(cwd); memcpy(dest,cwd,len); }
    if (!len) dest[len++]='/';
    while (*path) {
        while (*path=='/') ++path;
        start=0;
        while (path[start] && path[start]!='/') ++start;
        if (!start) break;
        if (start==1 && path[0]=='.') { path+=start; continue; }
        if (start==2 && path[0]=='.' && path[1]=='.') {
            while (len>1 && dest[len-1]!='/') --len;
            if (len>1) --len;
        } else {
            n=len+(len>1?1:0);
            if (n+start>=sizeof(dest)) { out("path too long\n"); return; }
            if (len>1) dest[len++]='/';
            memcpy(dest+len,path,start); len+=start;
        }
        path+=start;
    }
    dest[len]=0;
    if (call(10,(long)original,0,0,0)<0) failure();
    else memcpy(cwd,dest,len+1);
}
int main(void) {
    int n,i,overflow;
    unsigned pos;
    long count;
    char chunk[256];
    if (open("/dev/tty1",2)!=0 || call(17,0,0,0,0)!=1 || call(17,0,0,0,0)!=2)
        return 1;
    out("\nFUZIX on Brainfuck.\nType help for commands.\n");
    for (;;) {
        out("fuzix-bf# ");
        pos=0; overflow=0;
        do {
            count=read(0,chunk,sizeof(chunk));
            if (count<=0) break;
            for (i=0;i<count;++i) {
                if (chunk[i]=='\n') break;
                if (pos<sizeof(line)-1) line[pos++]=chunk[i];
                else overflow=1;
            }
        } while (count==sizeof(chunk) && chunk[count-1]!='\n');
        if (count<0) { failure(); continue; }
        if (!count && !pos && !overflow) { user_exit(); return 0; }
        if (overflow) { out("line too long (255 characters maximum)\n"); continue; }
        line[pos]=0;
        n=tokenize();
        if (!n) continue;
        if (!strcmp(args[0],"help"))
            out("Commands: help, echo text [> or >> file], pwd, cd [dir], ps, status\n"
                "          cat [file...], ls [dir...], uname [-amrsv], shutdown, exit\n");
        else if (!strcmp(args[0],"echo")) echo(n);
        else if (!strcmp(args[0],"status")) { number(last_status); out("\n"); }
        else if (!strcmp(args[0],"pwd")) { out(cwd); out("\n"); }
        else if (!strcmp(args[0],"cd") && n<=2) cd(n==2?args[1]:"/");
        else if (!strcmp(args[0],"ps")) {
            out("shell pid "); number(call(18,0,0,0,0)); out("\n");
        } else if (!strcmp(args[0],"shutdown") || !strcmp(args[0],"exit")) user_exit();
        else run();
    }
}
