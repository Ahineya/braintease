/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/wait.h>
#define CHECK(x) do {if(!(x)){printf("MOUNT_FAIL:%d errno=%d\n",__LINE__,errno);return 1;}} while(0)
int main(int argc,char **argv) {
    if(argc==2&&!strcmp(argv[1],"--setup")) {
        if(mkdir("/mnt",0755)&&errno!=EEXIST)return 1;
        if(mkdir("/mnt/remote",0755)&&errno!=EEXIST)return 1;
        puts("SFTP_SETUP_OK");return 0;
    }
    if(argc==2&&!strcmp(argv[1],"--names")) {
        DIR *d;struct dirent *entry;int fd;
        d=opendir("/mnt/remote/badnames");CHECK(d!=NULL);errno=0;
        while((entry=readdir(d))!=NULL);CHECK(errno==ENAMETOOLONG);CHECK(closedir(d)==0);
        errno=0;fd=open("/mnt/remote/nnnnnnnnnnnnnnnnnnnnnnnnnnnnnnn",O_WRONLY|O_TRUNC);
        CHECK(fd<0&&errno==ENAMETOOLONG);puts("SFTP_NAMES_OK");return 0;
    }
    if(argc==2&&!strcmp(argv[1],"--unsupported")) {
        int fd;char data[8];
        fd=open("/mnt/remote/temp",O_WRONLY|O_CREAT|O_EXCL,0600);CHECK(fd>=0);
        CHECK(write(fd,"new",3)==3);errno=0;CHECK(close(fd)<0&&(errno==EOPNOTSUPP||errno==EACCES));
        errno=0;CHECK(rename("/mnt/remote/temp","/mnt/remote/original")<0&&(errno==EOPNOTSUPP||errno==EACCES));
        fd=open("/mnt/remote/original",O_RDONLY);CHECK(fd>=0);
        CHECK(read(fd,data,4)==4&&!memcmp(data,"KEEP",4)&&close(fd)==0);
        puts("SFTP_UNSUPPORTED_OK");return 0;
    }
    unsigned char data[8192],copy[8192];char cwd[512];struct stat st;DIR *d;struct dirent *de;
    int fd,n,at,seen=0,pid,status,i;unsigned j;
    CHECK(chdir("/mnt/remote")==0);
    CHECK(getcwd(cwd,sizeof(cwd))!=NULL&&!strcmp(cwd,"/mnt/remote"));
    CHECK(stat("original",&st)==0&&st.st_size==8192);
    fd=open("original",O_RDONLY);CHECK(fd>=0);at=0;
    while((n=read(fd,copy+at,sizeof(copy)-at))>0)at+=n;
    CHECK(n==0&&at==8192&&close(fd)==0);
    for(j=0;j<8192;j++){CHECK(copy[j]==(j&255));data[j]=(255-j)&255;}
    CHECK(mkdir("newdir",0700)==0);
    fd=open("newdir/file",O_WRONLY|O_CREAT|O_EXCL,0600);CHECK(fd>=0);at=0;
    while(at<8192){n=write(fd,data+at,8192-at);CHECK(n>0);at+=n;}CHECK(close(fd)==0);
    fd=open("newdir/file",O_WRONLY|O_APPEND);CHECK(fd>=0);CHECK(write(fd,"tail",4)==4);CHECK(close(fd)==0);
    CHECK(stat("newdir/file",&st)==0&&st.st_size==8196);
    fd=open("replacement",O_WRONLY|O_CREAT|O_EXCL,0600);CHECK(fd>=0);
    CHECK(write(fd,"atomic",6)==6&&close(fd)==0);
    CHECK(rename("replacement","newdir/file")==0);
    CHECK(chmod("newdir/file",0644)==0);
    CHECK(stat("newdir/file",&st)==0&&st.st_size==6&&(st.st_mode&0777)==0644);
    CHECK(rename("newdir","moved")==0);
    CHECK(stat("moved/file",&st)==0&&st.st_size==6);
    d=opendir(".");CHECK(d!=NULL);
    errno=0;while((de=readdir(d))!=NULL){if(!strcmp(de->d_name,"moved"))seen=1;}CHECK(errno==0&&seen&&closedir(d)==0);
    /* The daemon, local shell and two clients must all be schedulable while
     * path walks/read/write calls block on different remote requests. */
    pid=fork();CHECK(pid>=0);
    if(!pid){for(i=0;i<12;i++){fd=open("original",O_RDONLY);if(fd<0||read(fd,copy,17)!=17||close(fd))_exit(2);}_exit(0);}
    for(i=0;i<12;i++){CHECK(stat("moved/file",&st)==0);CHECK(stat("/bin/sh",&st)==0);}
    CHECK(waitpid(pid,&status,0)==pid&&status==0);
    fd=open("moved/file",O_RDWR);CHECK(fd>=0&&ftruncate(fd,3)==0&&close(fd)==0);
    CHECK(unlink("moved/file")==0&&rmdir("moved")==0);
    /* Open-file identity survives unlink and an overwrite rename. */
    fd=open("orphan",O_RDWR|O_CREAT|O_EXCL,0600);CHECK(fd>=0);
    CHECK(write(fd,"before",6)==6&&unlink("orphan")==0);
    CHECK(lseek(fd,0,SEEK_SET)==0&&read(fd,copy,6)==6&&!memcmp(copy,"before",6));
    CHECK(ftruncate(fd,3)==0&&fstat(fd,&st)==0&&st.st_size==3&&close(fd)==0);
    fd=open("original",O_RDWR);CHECK(fd>=0);
    n=open("incoming",O_WRONLY|O_CREAT|O_EXCL,0644);CHECK(n>=0);
    CHECK(write(n,"after",5)==5&&close(n)==0&&rename("incoming","original")==0);
    CHECK(read(fd,copy,5)==5&&copy[0]==0&&copy[1]==1);
    CHECK(ftruncate(fd,2)==0&&fstat(fd,&st)==0&&st.st_size==2);
    CHECK(fchmod(fd,0600)==0&&close(fd)==0);
    CHECK(stat("original",&st)==0&&st.st_size==5&&(st.st_mode&0777)==0644);
    n=open("original",O_RDONLY);CHECK(n>=0&&read(n,copy,5)==5&&!memcmp(copy,"after",5)&&close(n)==0);
    CHECK(chdir("/")==0);
    puts("SFTP_MOUNT_OK");return 0;
}
