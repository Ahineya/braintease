/* Guest syscall acceptance probe, shared by RVM and Fuse-X. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/stat.h>
static int checks;
#define C(x) do { checks++; if (!(x)) { printf("SYMLINK_FAIL line %d errno %d: %s\n",__LINE__,errno,#x); return 1; } } while (0)
static int contents(const char *path, const char *wanted) {
    char b[32]; int fd=open(path,O_RDONLY), n;
    if(fd<0)return 0;
    n=read(fd,b,sizeof(b));close(fd);
    return n==(int)strlen(wanted) && !memcmp(b,wanted,n);
}
int main(int argc,char **argv) {
    struct stat st, target, before;
    char b[600], longpath[512], linkname[64], previous[64]; int fd, n;
    if(argc>1 && !strcmp(argv[1],"persist")) {
        C(lstat("/links/relative",&st)==0 && S_ISLNK(st.st_mode));
        C(contents("/links/relative","saved"));
        C(lstat("/links/dangling",&st)==0 && S_ISLNK(st.st_mode));
        C(stat("/links/dangling",&st)<0 && errno==ENOENT);
        puts("SYMLINK_PERSIST_OK"); return 0;
    }
    C(sysconf(_SC_SYMLOOP_MAX)==40);
    C(mkdir("/links",0755)==0);
    C(mkdir("/links/dir/",0755)==0);
    fd=open("/links/dir/file",O_CREAT|O_WRONLY,0644);C(fd>=0);
    C(write(fd,"saved",5)==5);C(close(fd)==0);
    C(stat("/links/dir/file",&target)==0);
    C(symlink("dir/file","/links/relative")==0);
    C(stat("/links/relative",&st)==0 && st.st_ino==target.st_ino);
    C(lstat("/links/relative",&st)==0 && S_ISLNK(st.st_mode) && st.st_size==8);
    C(contents("/links/relative","saved"));
    memset(b,'!',sizeof(b));C(readlink("/links/relative",b,3)==3 && !memcmp(b,"dir!",4));
    C(readlink("/links/relative",b,0)<0 && errno==EINVAL);
    C(readlink("/links/relative",(void *)1,8)<0 && errno==EFAULT);
    C(readlink("/links/dir/file",b,sizeof(b))<0 && errno==EINVAL);
    C(symlink("/links/dir","/links/absolute")==0);
    C(contents("/links/absolute/file","saved"));
    C(stat("/links/absolute/",&st)==0 && S_ISDIR(st.st_mode));
    C(lstat("/links/absolute/",&st)==0 && S_ISDIR(st.st_mode));
    C(chdir("/links/absolute")==0);
    C(symlink("../dir/file","parent")==0);
    C(contents("parent","saved"));C(chdir("/")==0);
    C(symlink("relative","/links/chain")==0);
    C(contents("/links/chain","saved"));
    C(symlink("missing","/links/dangling")==0);
    C(stat("/links/dangling",&st)<0 && errno==ENOENT);
    C(lstat("/links/dangling",&st)==0 && S_ISLNK(st.st_mode));
    C(symlink("other","/links/dangling")<0 && errno==EEXIST);
    C(link("/links/dir/file","/links/dangling")<0 && errno==EEXIST);
    C(open("/links/dangling",O_CREAT|O_EXCL|O_WRONLY,0600)<0 && errno==EEXIST);
    C(mkdir("/links/dangling",0700)<0 && errno==EEXIST);
    C(mknod("/links/dangling",S_IFIFO|0600,0)<0 && errno==EEXIST);
    C(symlink("new","/links/create")==0);
    fd=open("/links/create",O_CREAT|O_WRONLY,0600);C(fd>=0);C(write(fd,"new",3)==3);C(close(fd)==0);
    C(contents("/links/new","new"));C(unlink("/links/create")==0);C(contents("/links/new","new"));
    C(link("/links/relative","/links/hard-link")==0);
    C(lstat("/links/hard-link",&st)==0 && S_ISLNK(st.st_mode) && st.st_nlink==2);
    C(unlink("/links/hard-link")==0);
    C(symlink("dir/file","/links/renamed")==0);
    C(rename("/links/renamed","/links/moved")==0);
    C(lstat("/links/renamed",&st)<0 && errno==ENOENT);
    C(lstat("/links/moved",&st)==0 && S_ISLNK(st.st_mode));
    C(rename("/links/moved","/links/moved")==0);
    C(symlink("nope","/links/overwrite")==0);
    C(rename("/links/moved","/links/overwrite")==0);
    C(contents("/links/overwrite","saved"));
    C(unlink("/links/overwrite")==0);
    C(contents("/links/dir/file","saved"));
    C(unlink("/links/absolute/")<0 && errno==ENOTDIR);
    C(rmdir("/links/absolute")<0 && errno==ENOTDIR);
    C(rmdir("/links/absolute/")<0 && errno==ENOTDIR);
    C(rename("/links/absolute/","/links/bad")<0 && errno==ENOTDIR);
    C(stat("/links/relative/",&st)<0 && errno==ENOTDIR);
    C(open("/links/newfile/",O_CREAT|O_WRONLY,0600)<0 && errno==ENOENT);
    C(symlink("cycle-b","/links/cycle-a")==0);
    C(symlink("cycle-a","/links/cycle-b")==0);
    C(stat("/links/cycle-a",&st)<0 && errno==ELOOP);
    C(open("/links/cycle-a",O_CREAT|O_WRONLY,0600)<0 && errno==ELOOP);
    C(mkdir("/links/cycle-a/sub",0700)<0 && errno==ELOOP);
    C(mknod("/links/cycle-a/sub",S_IFIFO|0600,0)<0 && errno==ELOOP);
    C(symlink("file","/links/cycle-a/sub")<0 && errno==ELOOP);
    C(unlink("/links/cycle-a")==0);C(unlink("/links/cycle-b")==0);
    C(symlink("self","/links/self")==0);
    C(stat("/links/self",&st)<0 && errno==ELOOP);C(unlink("/links/self")==0);
    strcpy(previous,"dir/file");
    for(n=0;n<41;n++) {
        sprintf(linkname,"/links/depth-%d",n);
        C(symlink(previous,linkname)==0);
        sprintf(previous,"depth-%d",n);
        if(n==39)C(contents(linkname,"saved"));
    }
    C(stat(linkname,&st)<0 && errno==ELOOP);
    for(n=0;n<41;n++) {sprintf(linkname,"/links/depth-%d",n);C(unlink(linkname)==0);}
    memset(longpath,'x',511);longpath[511]=0;
    C(symlink(longpath,"/links/long")==0);
    C(readlink("/links/long",b,sizeof(b))==511 && !memcmp(b,longpath,511));
    C(stat("/links/long/tail",&st)<0 && errno==ENAMETOOLONG);
    C(unlink("/links/long")==0);
    memset(b,'x',512);b[512]=0;C(symlink(b,"/links/too-long")<0 && errno==ENAMETOOLONG);
    C(symlink("","/links/empty")<0 && errno==ENOENT);
    C(symlink((void *)1,"/links/invalid")<0 && errno==EFAULT);
    C(stat("",&st)<0 && errno==ENOENT);
    /* Repeated deletion must reclaim blocks, inodes and cache references. */
    for(n=0;n<100;n++) {C(symlink("relative","/links/reused")==0);C(contents("/links/reused","saved"));C(unlink("/links/reused")==0);}
    C(stat("/links/dir/file",&st)==0 && st.st_ino==target.st_ino && st.st_nlink==target.st_nlink);
    C(lstat("/bin/vi",&st)==0 && S_ISLNK(st.st_mode));
    C(stat("/bin/vi",&st)==0 && stat("/bin/rift",&before)==0 && st.st_ino==before.st_ino);
    C(lstat("/bin/ln",&st)==0 && S_ISLNK(st.st_mode));
    if(argc>1 && !strcmp(argv[1],"host")) {
        C(stat("/mnt/host/file",&st)==0);
        C(lstat("/mnt/host/hidden",&st)<0);
        C(open("/mnt/host/hidden",O_RDONLY)<0);
        C(symlink("file","/mnt/host/newlink")<0 && errno==EOPNOTSUPP);
        C(symlink("/mnt/host/file","/links/host-file")==0);
        C(contents("/links/host-file","host"));
        C(rename("/links/host-file","/links/host-moved")==0);
        C(unlink("/links/host-moved")==0);C(contents("/mnt/host/file","host"));
        C(symlink("/mnt/host","/links/host-dir")==0);
        C(stat("/links/host-dir/../host/file",&st)==0);
    }
    printf("SYMLINK_OK %d checks\n",checks);return 0;
}
