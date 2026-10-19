#include "kernel.h"
#include <assert.h>
#include <stdio.h>
struct cinode i_tab[ITABSIZE];
struct blkbuf bufpool[NBUFS];
struct io_udata udata;
static unsigned masked, allocations, wakes, signals, fault, sleep_action, disk_reads, fail_alloc;
unsigned di(void){unsigned old=masked;masked=1;return old;}
void irqrestore(unsigned n){masked=n;}
void *rcc_process_kernel_alloc(unsigned n){allocations++;return fail_alloc ? NULL : calloc(n,1);}
int _uget(const uint8_t *a,uint8_t *b,unsigned n){if(fault){udata.u_error=EFAULT;return -1;}memcpy(b,a,n);return 0;}
int _uput(const uint8_t *a,uint8_t *b,unsigned n){return _uget(a,b,n);}
unsigned umove(unsigned n){udata.u_done+=n;udata.u_count-=n;udata.u_offset+=n;udata.u_base+=n;return udata.u_done;}
void wakeup(void *p){(void)p;wakes++;}
void selwake_inode(inoptr p,unsigned m){(void)p;(void)m;}
void ssig(void *p,unsigned n){(void)p;assert(n==SIGPIPE);signals++;}
int psleep_flags(void *p,unsigned f){
 if(f&O_NDELAY){udata.u_error=EAGAIN;return -1;}
 if(sleep_action==1){((inoptr)p)->c_node.i_size=0;sleep_action=0;return 0;}
 udata.u_error=EINTR;return -1;
}
unsigned inode_blocks(inoptr p){return p->c_node.i_size/BLKSIZE;}
unsigned bmap(inoptr p,unsigned b,unsigned r){(void)p;(void)r;return b;}
bufptr bread(unsigned d,unsigned b,unsigned r){(void)d;(void)r;disk_reads++;assert(b<NBUFS);return &bufpool[b];}
void brelse(bufptr b){(void)b;}
bool namecomp(uint8_t *a,uint8_t *b){return !strncmp((char *)a,(char *)b,FILENAME_LEN);}
void namecpy(uint8_t *a,uint8_t *b,unsigned n){memset(a,0,n);memcpy(a,b,min(n,strlen((char *)b)));}
static unsigned writes[3],flushes[3],failed_write,failed_flush;
int bdwrite(bufptr b){writes[b->bf_dev]++;return failed_write && b==bufpool ? -1 : BLKSIZE;}
int d_flush(uint16_t dev){flushes[dev]++;return failed_flush && dev==1;}
#include "../../ripple/kernel-io.c"
#include "../../ripple/pipes.c"
#include "../../ripple/namecache.inc"
#include "../../ripple/bufindex.inc"
static void io(inoptr ino,unsigned write,uint8_t *data,unsigned n,unsigned f){udata.u_error=0;udata.u_base=data;udata.u_count=n;ripple_pipe_io(ino,write,f);assert(!masked);}
static void queues(void){
 uint8_t space[7],src[12],dst[12];unsigned i;
 struct s_queue q={space,space,space,7,0,0};
 for(i=0;i<12;i++)src[i]=i;
 assert(ripple_queue_write(&q,src,5,0)==5);
 assert(ripple_queue_read(&q,dst,3,0)==3&&!memcmp(dst,src,3));
 assert(ripple_queue_write(&q,src+5,6,0)==5&&q.q_count==7);
 fault=1;assert(ripple_queue_read(&q,dst,7,0)==-1&&q.q_count==7);fault=0;
 assert(ripple_queue_read(&q,dst,12,0)==7&&!memcmp(dst,src+3,7));
 fault=1;assert(ripple_queue_write(&q,src,7,0)==-1&&!q.q_count);fault=0;
 assert(ripple_queue_write(&q,src,12,1)==7);
 assert(ripple_queue_read(&q,dst,12,1)==7&&!memcmp(src,dst,7));
}
static void pipes(void){
 uint8_t src[9000],dst[9000];unsigned i,allocated;inoptr ino;
 for(i=0;i<9000;i++)src[i]=i;
 ino=ripple_pipe_new();assert(ino&&ino->c_dev==NO_DEVICE);
 ino->c_readers=ino->c_writers=1;
 io(ino,1,src,8000,0);assert(udata.u_done==8000&&ino->c_node.i_size==8000);
 io(ino,1,src,300,O_NDELAY);assert(udata.u_error==EAGAIN&&ino->c_node.i_size==8000);
 io(ino,0,dst,7999,0);assert(udata.u_done==7999&&!memcmp(src,dst,7999));
 io(ino,1,src,8191,0);assert(udata.u_done==8191&&ino->c_node.i_size==8192);
 io(ino,0,dst,9000,0);assert(udata.u_done==8192&&dst[0]==src[7999]&&!memcmp(dst+1,src,8191));
 io(ino,0,dst,1,O_NDELAY);assert(udata.u_error==EAGAIN);
 fault=1;io(ino,1,src,100,0);assert(udata.u_error==EFAULT&&!ino->c_node.i_size);fault=0;
 io(ino,1,src,100,0);fault=1;io(ino,0,dst,100,0);assert(udata.u_error==EFAULT&&ino->c_node.i_size==100);fault=0;
 io(ino,0,dst,100,0);assert(udata.u_done==100&&!memcmp(src,dst,100));
 ino->c_writers=0;io(ino,0,dst,100,0);assert(!udata.u_done&&!udata.u_error);
 ino->c_readers=0;io(ino,1,src,1,0);assert(udata.u_error==EPIPE&&signals==1);
 ino->c_readers=ino->c_writers=1;
 io(ino,1,src,8192,0);sleep_action=1;io(ino,1,src,100,0);assert(udata.u_done==100);
 io(ino,1,src,8192,0);assert(udata.u_error==EINTR); /* no scheduler action */
 allocated=allocations;ripple_pipe_release(ino);memset(ino,0,sizeof(*ino));
 ino=ripple_pipe_new();ino->c_readers=ino->c_writers=1;io(ino,1,src,8192,0);assert(allocations==allocated);
 ripple_pipe_release(ino);
 i_tab[0].c_refs=1;ino=ripple_pipe_new();ino->c_readers=ino->c_writers=1;
 io(ino,1,src,1,0);assert(udata.u_done==1&&allocations==allocated);
 ino=ripple_pipe_new();ino->c_readers=ino->c_writers=1;
 fail_alloc=1;io(ino,1,src,1,0);assert(udata.u_error==ENOMEM&&!ino->c_node.i_size);
 fail_alloc=0;io(ino,1,src,1,0);assert(udata.u_done==1);
 for(i=0;i<ITABSIZE;i++)i_tab[i].c_refs=1;
 udata.u_error=0;assert(!ripple_pipe_new()&&udata.u_error==ENFILE);
}
static void caches(void){
 struct cinode dir={0};off_t at;struct name_entry *e;unsigned n;
 struct direct *d=(void *)bufpool[0].__bf_data;
 dir.c_dev=2;dir.c_num=7;dir.c_node.i_size=BLKSIZE;
 d[0].d_ino=12;strcpy((char *)d[0].d_name,"hello");
 udata.u_error=0;
 assert(directory_find(&dir,(void *)"hello",&at)==12&&at==0);
 name_remember(&dir,(void *)"hello",12,0);e=name_lookup(&dir,(void *)"hello");assert(e&&e->number==12);
 assert(!directory_find(&dir,(void *)"missing",&at)&&at==DIR_LEN);
 name_remember(&dir,(void *)"missing",0,at);assert(name_lookup(&dir,(void *)"missing"));
 name_forget(&dir);assert(!name_lookup(&dir,(void *)"hello")&&!name_lookup(&dir,(void *)"missing"));
 name_remember(&dir,(void *)"hello",12,0);ripple_namecache_drop(2);assert(!name_lookup(&dir,(void *)"hello"));
 for(n=0;n<NBUFS;n++){bufpool[n].bf_dev=1;bufpool[n].bf_blk=n*BUFFER_BUCKETS;buffer_insert(&bufpool[n]);}
 assert(buffer_head[buffer_bucket(1,0)]==NBUFS);
 buffer_remove(&bufpool[3]);buffer_remove(&bufpool[0]);buffer_remove(&bufpool[NBUFS-1]);
 n=buffer_head[buffer_bucket(1,0)];unsigned count=0;
 while(n){assert(n!=1&&n!=4&&n!=NBUFS);count++;n=buffer_next[n-1];}assert(count==NBUFS-3);
 bufpool[3].bf_blk=1;buffer_insert(&bufpool[3]);assert(buffer_head[buffer_bucket(1,1)]==4);
 assert(disk_reads==2);
}
static void barriers(void){
 unsigned n;
 for(n=0;n<NBUFS;n++){bufpool[n].bf_dev=n%2+1;bufpool[n].bf_dirty=1;}
 udata.u_error=0;ripple_bufsync();
 assert(writes[1]==4&&writes[2]==4&&flushes[1]==1&&flushes[2]==1);
 for(n=0;n<NBUFS;n++)assert(!bufpool[n].bf_dirty);
 for(n=0;n<NBUFS;n++)bufpool[n].bf_dirty=1;
 memset(writes,0,sizeof(writes));memset(flushes,0,sizeof(flushes));
 failed_flush=1;ripple_bufsync();assert(udata.u_error==EIO&&flushes[1]==1&&flushes[2]==1);
 for(n=0;n<NBUFS;n++)assert(bufpool[n].bf_dirty==(n%2==0));
 failed_flush=0;failed_write=1;udata.u_error=0;ripple_bufsync();assert(udata.u_error==EIO&&bufpool[0].bf_dirty);
 for(n=1;n<NBUFS;n++)assert(!bufpool[n].bf_dirty);
 failed_write=0;ripple_bufsync();assert(!bufpool[0].bf_dirty);
}
int main(void){queues();pipes();caches();barriers();puts("KERNEL_IO_UNIT_OK");return 0;}
