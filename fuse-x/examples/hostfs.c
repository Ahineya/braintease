/* SPDX-License-Identifier: GPL-2.0-only */
/* Runs inside Riplix: descriptor ownership must remain in the guest kernel. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <dirent.h>
#include <utime.h>
static unsigned char data[8193],copy[8193];
static void require(int good,int line) {
    if(!good){printf("hostfs FAILED line %d errno %d\n",line,errno);exit(1);}
}
#define CHECK(x) require(!!(x),__LINE__)
int main(void) {
    int fd,other,child,status,n,i;
    struct stat a,b;
    struct utimbuf times;
    DIR *dir;struct dirent *entry;unsigned found=0;
    CHECK(chdir("/mnt/host")==0);
    CHECK(stat(".",&a)==0 && S_ISDIR(a.st_mode));
    for(i=0;i<sizeof(data);i++)data[i]=i&255;
    fd=open("input.bin",O_RDONLY);CHECK(fd>=0);
    n=0;while(n<sizeof(copy)){i=read(fd,copy+n,sizeof(copy)-n);CHECK(i>0);n+=i;}
    CHECK(!memcmp(data,copy,sizeof(copy)));CHECK(read(fd,copy,1)==0);CHECK(close(fd)==0);
    fd=open("created.bin",O_CREAT|O_EXCL|O_RDWR,0640);CHECK(fd>=0);
    CHECK(write(fd,data,sizeof(data))==sizeof(data));
    CHECK(lseek(fd,0,SEEK_SET)==0);CHECK(read(fd,copy,sizeof(copy))==sizeof(copy));
    CHECK(!memcmp(data,copy,sizeof(copy)));
    other=dup(fd);CHECK(other>=0);CHECK(lseek(fd,0,SEEK_SET)==0);
    child=fork();CHECK(child>=0);
    if(!child){CHECK(read(other,copy,2)==2 && copy[0]==0 && copy[1]==1);close(fd);close(other);_exit(17);}
    CHECK(waitpid(child,&status,0)==child && WIFEXITED(status) && WEXITSTATUS(status)==17);
    CHECK(read(fd,copy,2)==2 && copy[0]==2 && copy[1]==3);CHECK(close(other)==0);
    CHECK(rename("created.bin","renamed.bin")==0);
    CHECK(link("renamed.bin","linked.bin")==0);
    CHECK(stat("linked.bin",&a)==0 && fstat(fd,&b)==0 && a.st_ino==b.st_ino && a.st_nlink==2);
    CHECK(unlink("renamed.bin")==0);CHECK(unlink("linked.bin")==0);
    CHECK(lseek(fd,8192,SEEK_SET)==8192 && read(fd,copy,1)==1 && copy[0]==0);
    CHECK(write(fd,"X",1)==1);CHECK(close(fd)==0);
    fd=open("append.txt",O_CREAT|O_WRONLY|O_APPEND,0644);CHECK(fd>=0);
    CHECK(write(fd,"first",5)==5);CHECK(lseek(fd,0,SEEK_SET)==0);
    CHECK(write(fd,"second",6)==6);
    copy[0]=256;errno=0;CHECK(write(fd,copy,1)==-1 && errno==EINVAL);
    CHECK(close(fd)==0);
    times.actime=1800000000UL;times.modtime=1900000000UL;
    CHECK(utime("append.txt",&times)==0);
    fd=open("sparse.bin",O_CREAT|O_RDWR,0644);CHECK(fd>=0);
    CHECK(lseek(fd,20000,SEEK_SET)==20000 && write(fd,"Z",1)==1);
    CHECK(lseek(fd,19999,SEEK_SET)==19999 && read(fd,copy,2)==2 && copy[0]==0 && copy[1]=='Z');
    CHECK(ftruncate(fd,4)==0);CHECK(ftruncate(fd,9)==0);
    CHECK(fstat(fd,&a)==0 && a.st_size==9);CHECK(close(fd)==0);
    CHECK(chmod("sparse.bin",0600)==0);CHECK(stat("sparse.bin",&a)==0 && (a.st_mode&0777)==0600);
    CHECK(mkdir("directory",0755)==0);
    fd=open("directory/note",O_CREAT|O_WRONLY,0644);CHECK(fd>=0);CHECK(write(fd,"nested",6)==6);close(fd);
    errno=0;CHECK(rmdir("directory")==-1 && errno==ENOTEMPTY);
    CHECK(rename("directory","moved-directory")==0);
    CHECK(chdir("moved-directory")==0);CHECK(chdir("..")==0);
    CHECK(unlink("moved-directory/note")==0);CHECK(rmdir("moved-directory")==0);
    fd=open(".",O_RDONLY);
    CHECK(fd>=0);
    n=fstat(fd,&a);
    CHECK(n==0 && S_ISDIR(a.st_mode));
    CHECK(close(fd)==0);
    dir=opendir(".");CHECK(dir!=NULL);
    while((entry=readdir(dir)))if(!strcmp(entry->d_name,"append.txt"))found++;
    CHECK(found==1);CHECK(closedir(dir)==0);
    errno=0;CHECK(open("escape",O_RDONLY)==-1 && errno==EACCES);
    errno=0;CHECK(open("123456789012345678901234567890x",O_WRONLY|O_TRUNC)==-1 && errno==ENAMETOOLONG);
    errno=0;CHECK(rename("append.txt","/tmp/cross-mount")==-1 && errno==EXDEV);
    CHECK(chdir("..")==0);CHECK(stat("host/append.txt",&a)==0);
    CHECK(chdir("host")==0);
    CHECK(chmod("outside-program",0644)==0);
    child=fork();CHECK(child>=0);
    if(!child) {
        char *args[]={"outside-program",NULL},*env[]={NULL};
        execve("outside-program",args,env);_exit(errno==EACCES?23:99);
    }
    CHECK(waitpid(child,&status,0)==child && WIFEXITED(status) && WEXITSTATUS(status)==23);
    CHECK(chmod("outside-program",0755)==0);
    puts("hostfs: binary I/O, fork/dup, rename/unlink, append, sparse files, directories and boundaries passed");
    return 0;
}
