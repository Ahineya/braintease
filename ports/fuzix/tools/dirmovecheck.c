/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/wait.h>
static void failed(unsigned line){fprintf(stderr,"DIRMOVE_FAILED %u errno=%d\n",line,errno);exit(1);}
#define C(test) do{if(!(test))failed(__LINE__);}while(0)
static struct stat info(const char *path){struct stat s;C(lstat(path,&s)==0);return s;}
static void missing(const char *path){struct stat s;C(lstat(path,&s)<0&&errno==ENOENT);}
static void put(const char *path,const char *text){int fd=open(path,O_WRONLY|O_CREAT|O_EXCL,0644);C(fd>=0);C(write(fd,text,strlen(text))==strlen(text));C(close(fd)==0);}
static void content(const char *path,const char *text){char b[64];int fd=open(path,O_RDONLY),n;C(fd>=0);n=read(fd,b,sizeof(b));C(n==strlen(text)&&!memcmp(b,text,n));C(close(fd)==0);}
static void initial(void){
    struct stat tree,left,right,s;char cwd[512],path[80];int fd,i,pid,status;
    C(mkdir("/rename-test",0755)==0);C(mkdir("/rename-test/left",0777)==0);C(mkdir("/rename-test/right",0777)==0);
    C(chmod("/rename-test/left",0777)==0);C(chmod("/rename-test/right",0777)==0);
    C(mkdir("/rename-test/left/tree",0755)==0);C(mkdir("/rename-test/left/tree/deep",0755)==0);
    put("/rename-test/left/tree/deep/data","directory payload");
    C(link("/rename-test/left/tree/deep/data","/rename-test/alias")==0);
    C(symlink("deep/data","/rename-test/left/tree/link")==0);
    fd=open("/rename-test/left/tree/deep/data",O_RDONLY);C(fd>=0);
    tree=info("/rename-test/left/tree");left=info("/rename-test/left");right=info("/rename-test/right");
    C(rename("/rename-test/left/tree","/rename-test/left/renamed")==0);
    missing("/rename-test/left/tree");s=info("/rename-test/left/renamed");C(s.st_ino==tree.st_ino);
    s=info("/rename-test/left");C(s.st_nlink==left.st_nlink);
    C(chdir("/rename-test/left/renamed/deep")==0);
    C(rename("/rename-test/left/renamed","/rename-test/right/moved")==0);
    C(getcwd(cwd,sizeof(cwd))!=NULL&&!strcmp(cwd,"/rename-test/right/moved/deep"));
    C(chdir("..")==0);C(chdir("..")==0);C(getcwd(cwd,sizeof(cwd))!=NULL&&!strcmp(cwd,"/rename-test/right"));
    C(chdir("/")==0);
    s=info("/rename-test/right/moved");C(s.st_ino==tree.st_ino&&s.st_nlink==tree.st_nlink);
    s=info("/rename-test/left");C(s.st_nlink==left.st_nlink-1);
    s=info("/rename-test/right");C(s.st_nlink==right.st_nlink+1);
    C(read(fd,cwd,17)==17&&!memcmp(cwd,"directory payload",17));C(close(fd)==0);
    content("/rename-test/right/moved/link","directory payload");
    s=info("/rename-test/right/moved/deep/data");tree=info("/rename-test/alias");C(s.st_ino==tree.st_ino&&s.st_nlink==2);
    C(rename("/rename-test/right/moved","/rename-test/right/moved/deep/loop")<0&&errno==EINVAL);
    C(chdir("/rename-test/right/moved/deep")==0);
    C(rename("/rename-test/right/moved","relative-loop")<0&&errno==EINVAL);
    C(rename(".","/rename-test/bad-dot")<0&&errno==EINVAL);
    C(rename("..","/rename-test/bad-parent")<0&&errno==EINVAL);
    C(chdir("/")==0);
    C(rename("/","/rename-test/root")<0&&errno==EBUSY);
    C(rename("/rename-test/right/moved","/rename-test/right/.")<0&&errno==EINVAL);
    C(rename("/rename-test/right/moved","/rename-test/missing/child")<0&&errno==ENOENT);
    C(mkdir("/rename-test/right/full",0755)==0);put("/rename-test/right/full/keep","KEEP");
    C(rename("/rename-test/right/moved","/rename-test/right/full")<0&&errno==ENOTEMPTY);
    put("/rename-test/file","FILE");
    C(rename("/rename-test/right/moved","/rename-test/file")<0&&errno==ENOTDIR);
    C(rename("/rename-test/file","/rename-test/right/moved")<0&&errno==EISDIR);
    C(rename("/rename-test/right/moved","/rename-test/right/moved")==0);
    /* Visiting the source and leaving it via .. is not a descendant move. */
    C(rename("/rename-test/right/moved","/rename-test/right/moved/../out-and-back")==0);
    C(rename("/rename-test/right/out-and-back","/rename-test/right/moved")==0);
    C(mkdir("/rename-test/left/empty",0755)==0);
    left=info("/rename-test/left");right=info("/rename-test/right");tree=info("/rename-test/right/moved");
    C(rename("/rename-test/right/moved","/rename-test/left/empty")==0);
    s=info("/rename-test/left/empty");C(s.st_ino==tree.st_ino);
    s=info("/rename-test/left");C(s.st_nlink==left.st_nlink);
    s=info("/rename-test/right");C(s.st_nlink==right.st_nlink-1);
    C(mkdir("/rename-test/left/replacement",0755)==0);left=info("/rename-test/left");
    C(rename("/rename-test/left/empty","/rename-test/left/replacement")==0);
    s=info("/rename-test/left");C(s.st_nlink==left.st_nlink-1);
    C(mkdir("/rename-test/left/readonly",0555)==0);C(chmod("/rename-test/left/readonly",0555)==0);
    pid=fork();C(pid>=0);
    if(!pid){C(setgid(1)==0&&setuid(1)==0);
        C(rename("/rename-test/left/readonly","/rename-test/right/readonly")==0);_exit(0);}
    C(waitpid(pid,&status,0)==pid&&status==0);
    C(chmod("/rename-test/right",0555)==0);pid=fork();C(pid>=0);
    if(!pid){C(setgid(1)==0&&setuid(1)==0);
        C(rename("/rename-test/right/readonly","/rename-test/left/forbidden")<0&&errno==EACCES);_exit(0);}
    C(waitpid(pid,&status,0)==pid&&status==0);
    C(chmod("/rename-test/right",0777)==0);missing("/rename-test/left/forbidden");
    /* Repeated same-parent renames must not leak inode refs or link counts. */
    tree=info("/rename-test/left/replacement");
    for(i=0;i<40;i++){
        C(rename("/rename-test/left/replacement","/rename-test/left/temporary")==0);
        C(rename("/rename-test/left/temporary","/rename-test/left/replacement")==0);
    }
    s=info("/rename-test/left/replacement");C(s.st_ino==tree.st_ino&&s.st_nlink==tree.st_nlink);
    /* A full directory block needs no additional slot for an in-place rename. */
    C(mkdir("/rename-test/packed",0755)==0);
    for(i=0;i<14;i++){sprintf(path,"/rename-test/packed/d%02d",i);C(mkdir(path,0755)==0);}
    left=info("/rename-test/packed");C(rename("/rename-test/packed/d00","/rename-test/packed/new-name")==0);
    s=info("/rename-test/packed");C(s.st_size==left.st_size&&s.st_nlink==left.st_nlink);
    C(mkdir("/cmd-left",0755)==0);C(mkdir("/cmd-right",0755)==0);
    C(mkdir("/cmd-left/tree",0755)==0);C(mkdir("/cmd-left/tree/deep",0755)==0);put("/cmd-left/tree/deep/data","Commander directory move\n");
    puts("DIRMOVE_OK");
}
int main(int argc,char **argv){
    if(argc>1){
        content("/rename-test/left/replacement/deep/data","directory payload");
        content("/rename-test/left/replacement/link","directory payload");
        content("/rename-test/right/full/keep","KEEP");content("/rename-test/alias","directory payload");
        content("/cmd-left/tree/deep/data","Commander directory move\n");
        missing("/cmd-right/tree");missing("/cmd-right/renamed tree");
        puts("DIRMOVE_PERSIST_OK");return 0;
    }
    initial();return 0;
}
