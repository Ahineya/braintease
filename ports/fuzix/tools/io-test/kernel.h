#ifndef IO_TEST_KERNEL_H
#define IO_TEST_KERNEL_H
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#define CONFIG_RIPPLE_IO
#define CONFIG_FUSE_X
#define ITABSIZE 16
#define BLKSIZE 1024
#define NBUFS 8
#define FILENAME_LEN 30
#define DIR_LEN 32
#define CMAGIC 24721
#define NULLINODE ((inoptr)0)
#define NO_DEVICE 65535
#define SELECT_IN 1
#define SELECT_OUT 2
#define SIGPIPE 13
#define O_NDELAY 16
#define min(a,b) ((a)<(b)?(a):(b))
#define BLK_TO_OFFSET(x) ((x)*BLKSIZE)
typedef unsigned irqflags_t;
typedef unsigned usize_t;
typedef unsigned blkno_t;
typedef struct cinode {
 unsigned c_magic,c_dev,c_num,c_refs,c_readers,c_writers;
 struct { unsigned i_size,i_mode; } c_node;
} *inoptr;
struct s_queue {uint8_t *q_base,*q_head,*q_tail;int q_size,q_count,q_wakeup;};
struct blkbuf {uint8_t __bf_data[BLKSIZE];unsigned bf_dev,bf_blk,bf_dirty;};
typedef struct blkbuf *bufptr;
struct direct {uint16_t d_ino;uint8_t d_name[30];};
extern struct cinode i_tab[ITABSIZE];
extern struct blkbuf bufpool[NBUFS];
#define bufpool_end (bufpool+NBUFS)
int bdwrite(bufptr);
int d_flush(uint16_t);
struct io_udata {unsigned u_done,u_count,u_sysio,u_error,u_offset;uint8_t *u_base;void *u_ptab;};
extern struct io_udata udata;
unsigned di(void);
void irqrestore(unsigned);
void *ripple_kernel_alloc(unsigned);
int _uget(const uint8_t *,uint8_t *,unsigned);
int _uput(const uint8_t *,uint8_t *,unsigned);
unsigned umove(unsigned);
void wakeup(void *);
void selwake_inode(inoptr,unsigned);
void ssig(void *,unsigned);
int psleep_flags(void *,unsigned);
unsigned inode_blocks(inoptr);
unsigned bmap(inoptr,unsigned,unsigned);
bufptr bread(unsigned,unsigned,unsigned);
void brelse(bufptr);
#define blkptr(b,o,n) ((void *)((b)->__bf_data+(o)))
bool namecomp(uint8_t *,uint8_t *);
void namecpy(uint8_t *,uint8_t *,unsigned);
#endif
