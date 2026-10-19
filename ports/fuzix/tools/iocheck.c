/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Real-kernel IO correctness and optional timings, on disposable disks only. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <dirent.h>
#include <signal.h>
#include <termios.h>
#include <sys/stat.h>
#include <sys/select.h>
#include <sys/wait.h>
#include <time.h>
extern long call(unsigned,long,long,long,long);
static char data[9000], copy[9000];
static void fail(unsigned line){printf("IO_FAILED %u errno %d\n",line,errno);exit(1);}
#define C(x) do{if(!(x))fail(__LINE__);}while(0)
static void stamp(struct timespec *t){C(!clock_gettime(CLOCK_MONOTONIC,t));}
static void measure(char *name,unsigned count,struct timespec *a,struct timespec *b){
 unsigned long us=(b->tv_sec-a->tv_sec)*1000000UL+(b->tv_nsec-a->tv_nsec)/1000L;
 printf("MEASURE %s %u %lu\n",name,count,us);fflush(stdout);
}
static void put(char *p){int fd=open(p,O_CREAT|O_EXCL|O_WRONLY,0644);C(fd>=0);C(write(fd,"contents",8)==8);C(!close(fd));}
static void pipes(void){
 int fds[2],pid,status,n,i;unsigned done;struct stat metadata;
 C(!pipe(fds));
 data[0]=0x1234;C(write(fds[1],data,1)==1&&read(fds[0],copy,1)==1&&copy[0]==0x1234);data[0]=0;
 C(!fstat(fds[0],&metadata)&&metadata.st_atime>0&&metadata.st_mtime>0);
 C(!fcntl(fds[0],F_SETFL,O_NONBLOCK));
 C(read(fds[0],copy,1)==-1&&errno==EAGAIN);
 C(!fcntl(fds[1],F_SETFL,O_NONBLOCK));
 C(write(fds[1],data,8000)==8000);
 C(write(fds[1],data,300)==-1&&errno==EAGAIN); /* indivisible small write */
 C(read(fds[0],copy,7999)==7999&&!memcmp(data,copy,7999));
 C(write(fds[1],data,8191)==8191);C(read(fds[0],copy,9000)==8192);
 C(copy[0]==data[7999]&&!memcmp(copy+1,data,8191));
 C(!close(fds[1]));C(!read(fds[0],copy,1));C(!close(fds[0]));
 C(!pipe(fds));signal(SIGPIPE,SIG_IGN);C(!close(fds[0]));
 C(write(fds[1],data,1)==-1&&errno==EPIPE);C(!close(fds[1]));
 /* Multi-writer records must remain intact when the ring wraps or fills. */
 C(!pipe(fds));
 pid=fork();C(pid>=0);
 if(!pid){close(fds[0]);memset(data,'A',300);for(i=0;i<80;i++)C(write(fds[1],data,300)==300);_exit(0);}
 n=fork();C(n>=0);
 if(!n){close(fds[0]);memset(data,'B',300);for(i=0;i<80;i++)C(write(fds[1],data,300)==300);_exit(0);}
 close(fds[1]);
 for(i=0;i<160;i++){
  done=0;while(done<300){int got=read(fds[0],copy+done,300-done);C(got>0);done+=got;}
  C(copy[0]=='A'||copy[0]=='B');for(done=1;done<300;done++)C(copy[done]==copy[0]);
 }
 C(!read(fds[0],copy,1));close(fds[0]);C(waitpid(pid,&status,0)==pid&&!status);C(waitpid(n,&status,0)==n&&!status);
 puts("IO_PIPES_OK");
}
static void directories(void){
 DIR *d;struct dirent *e;struct stat st;char path[80],saved[31];long position;
 unsigned i,j,count;struct timespec a,b;
 C(!mkdir("/io-test",0755)||errno==EEXIST);
 stamp(&a);
 for(i=0;i<160;i++){sprintf(path,"/io-test/f%03u",i);put(path);}
 stamp(&b);measure("create_file",160,&a,&b);
 d=opendir("/io-test");C(d);C(readdir(d));position=telldir(d);C(position>=0);
 e=readdir(d);C(e);strcpy(saved,e->d_name);seekdir(d,position);e=readdir(d);C(e&&!strcmp(saved,e->d_name));
 rewinddir(d);C(!telldir(d));C(!closedir(d));
 stamp(&a);
 for(j=0;j<10;j++){
  d=opendir("/io-test");C(d);count=0;while((e=readdir(d)))count++;C(count==162);C(!closedir(d));
 }
 stamp(&b);measure("directory_entry",1620,&a,&b);
 stamp(&a);
 for(i=0;i<1000;i++)C(!stat("/io-test/f159",&st)&&st.st_size==8);
 stamp(&b);measure("lookup_cached",1000,&a,&b);
 /* Negative cache, create, rename, removal, inode recycling and hole reuse. */
 C(stat("/io-test/missing",&st)==-1&&errno==ENOENT);put("/io-test/missing");C(!stat("/io-test/missing",&st));
 C(!rename("/io-test/missing","/io-test/moved"));C(stat("/io-test/missing",&st)==-1&&errno==ENOENT);
 C(!stat("/io-test/moved",&st));C(!unlink("/io-test/moved"));
 for(i=0;i<160;i++){sprintf(path,"/io-test/f%03u",i);C(!unlink(path));}
 for(i=0;i<80;i++){put("/io-test/reused");C(!stat("/io-test/reused",&st));C(!unlink("/io-test/reused"));C(stat("/io-test/reused",&st)==-1&&errno==ENOENT);}
 C(!rmdir("/io-test"));puts("IO_DIRECTORIES_OK");
}
static void terminals(void){
 int master=open("/dev/ptyp0",O_RDWR|O_NOCTTY|O_NONBLOCK),slave;
 char *large;
 struct termios t;unsigned i,j;struct timespec a,b;
 C(master>=0);slave=open("/dev/ttyp0",O_RDWR|O_NOCTTY|O_NONBLOCK);C(slave>=0);
 C(!tcgetattr(slave,&t));t.c_lflag=0;t.c_iflag=0;t.c_oflag=0;t.c_cc[VMIN]=1;t.c_cc[VTIME]=0;
 C(!tcsetattr(slave,TCSANOW,&t));
 data[0]=0x1234;C(write(slave,data,1)==1&&read(master,copy,1)==1&&copy[0]==0x34);
 C(write(master,data,1)==1&&read(slave,copy,1)==1&&copy[0]==0x34);
 stamp(&a);
 for(i=0;i<200;i++){
  for(j=0;j<512;j++)data[j]=(i+j)&255;
  C(write(slave,data,512)==512);C(read(master,copy,333)==333&&!memcmp(data,copy,333));
  C(read(master,copy+333,179)==179&&!memcmp(data,copy,512));
  C(write(master,data,512)==512);C(read(slave,copy,512)==512&&!memcmp(data,copy,512));
 }
 stamp(&b);measure("pty_roundtrip_512",200,&a,&b);
 large=malloc(70000UL);C(large);memset(large,0,70000UL);
 C(write(master,data,512)==512);C(read(slave,large,65536UL)==512&&!memcmp(large,data,512));
 C(write(slave,large,65536UL)==512);C(read(master,copy,512)==512&&!memcmp(large,copy,512));free(large);
 t.c_oflag=OPOST|ONLCR;C(!tcsetattr(slave,TCSANOW,&t));
 C(write(slave,"ab\ncd\n",6)==6);C(read(master,copy,20)==8&&!memcmp(copy,"ab\r\ncd\r\n",8));
 close(slave);C(!read(master,copy,1));close(master);puts("IO_PTY_OK");
}
static void shared(void){
 int fd,i;struct stat s;struct timespec a,b;
 fd=open("/mnt/host/io-check",O_CREAT|O_RDWR|O_TRUNC,0644);C(fd>=0);
 stamp(&a);
 for(i=0;i<100;i++)C(write(fd,"abc",3)==3);
 C(lseek(fd,0,SEEK_END)==300);C(!fstat(fd,&s)&&s.st_size==300);C(!close(fd));
 stamp(&b);measure("host_write_3",100,&a,&b);
 fd=open("/mnt/host/io-check",O_WRONLY|O_APPEND|O_SYNC);C(fd>=0);C(write(fd,"!",1)==1);close(fd);
 fd=open("/mnt/host/io-check",O_RDONLY);C(fd>=0);C(!fstat(fd,&s)&&s.st_size==301);close(fd);
 sync();C(!unlink("/mnt/host/io-check"));puts("IO_SHARED_OK");
}
int main(int argc,char **argv){unsigned i;for(i=0;i<9000;i++)data[i]=i&255;
 if(argc>1&&!strcmp(argv[1],"persist")){int fd=open("/saved",O_RDONLY);C(fd>=0&&read(fd,copy,20)==10&&!memcmp(copy,"preserved\n",10));close(fd);puts("IO_PERSIST_OK");return 0;}
 if(argc>1&&!strcmp(argv[1],"readonly")){
  C(call(34,(long)"/dev/root",129,0,0)==0);
  C(open("/should-not-exist",O_CREAT|O_WRONLY,0644)==-1&&errno==EROFS);
  pipes();C(call(34,(long)"/dev/root",128,0,0)==0);puts("IO_READONLY_OK");return 0;
 }
 if(argc<2||strcmp(argv[1],"bench"))pipes();
 directories();terminals();if(argc>2)shared();puts("PROBE_OK");return 0;
}
