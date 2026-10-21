/* SPDX-License-Identifier: MIT */
#ifndef LVSELECT_BROWSER_H
#define LVSELECT_BROWSER_H
#include <stdint.h>
#define BROWSER_PATH 2048
#define BROWSER_NAME 256
#define BROWSER_LIMIT 1024
#define BROWSER_DIRECTORY 1
#define BROWSER_FILE 2
struct browser_entry {char name[BROWSER_NAME];unsigned long size;int kind;};
struct browser {
    char path[BROWSER_PATH];
    struct browser_entry *entries;
    unsigned count;
    int hidden,limited;
};
typedef struct browser_job browser_job;
/* Directory scans never change process cwd. They retain at most BROWSER_LIMIT
 * entries, preserve the old listing until commit, and include sorting in the
 * caller's work budget. Delete an unfinished job to cancel it. */
browser_job *browser_scan_begin(const char *path,int hidden);
/* 0 running, 1 ready, -1 error (errno retained by browser_scan_error). */
int browser_scan_step(browser_job *job,unsigned budget);
int browser_scan_commit(browser_job *job,struct browser *browser);
int browser_scan_error(const browser_job *job);
const char *browser_scan_path(const browser_job *job);
void browser_scan_delete(browser_job *job);
/* Synchronous non-mutating convenience wrapper. */
int browser_scan(struct browser *b,const char *path,int hidden);
/* A successful load changes this process's cwd to the canonical directory.
 * Failures preserve both the old listing and cwd. */
int browser_load(struct browser *b,const char *path,int hidden);
void browser_free(struct browser *b);
int browser_join(const char *directory,const char *name,char *out);
void browser_parent(const struct browser *b,char *out);
int browser_utf8(uint32_t scalar,char out[5]);
#endif
