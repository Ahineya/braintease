/* SPDX-License-Identifier: MIT */
#ifndef RIPLIX_SERVICE_H
#define RIPLIX_SERVICE_H
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <dirent.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/un.h>
#define SV_ARGS 8
#define SV_NAME 25
#define SV_PATH 256
#define SV_REPLY 256
#define SV_INPUT 128
#define SV_DEFINITIONS 8192
#define SV_NONE ((size_t)-1)
#define SV_STARTUPS 4
struct sv_links {size_t *item,count,capacity;};
enum { SV_DOWN,SV_WAITING,SV_STARTING,SV_RUNNING,SV_DONE,SV_STOPPING,SV_BACKOFF,SV_FAILED,SV_BLOCKED };
enum { SV_NO,SV_FAILURE,SV_ALWAYS };
struct service {
    char name[SV_NAME],program[SV_PATH],directory[SV_PATH],args[SV_ARGS][128];
    char *after,*requires;
    struct sv_links before,needs;
    unsigned argc,oneshot,notify,restart,restart_limit,start_ms,stop_ms,restart_ms;
    unsigned seen,arg_seen,desired,state,retries,want_stop;
    unsigned long deadline;
    int pid,exec_fd,ready_fd,exec_done,notified,last_exit,last_signal;
};
struct services {
    struct service *item;
    size_t count,capacity,*order;
    unsigned char *marks; /* Reusable graph traversal scratch; no allocation in control requests. */
};
char *sv_read_config(const char *,size_t *,char *,unsigned);
int sv_name(const char *);
size_t sv_find(struct services *,const char *);
int sv_has(const struct sv_links *,size_t);
void sv_free(struct services *);
int sv_load(struct services *,const char *,char *,unsigned);
int sv_profile(struct services *,const char *,char *,unsigned);
void sv_select(struct services *,size_t);
void sv_require(struct services *,size_t);
int sv_runtime(const char *,int,char *,unsigned);
#endif
