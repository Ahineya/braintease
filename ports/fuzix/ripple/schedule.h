/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef RIPLIX_SCHEDULE_H
#define RIPLIX_SCHEDULE_H
#include <sys/types.h>
#include <sys/stat.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <dirent.h>
#include <errno.h>
#include <signal.h>
#include <time.h>
#include <ctype.h>
#include <limits.h>

#define AT_SPOOL "/var/spool/at"
#define SCHED_EPOCH_MAX 4294967295UL
#define SCHED_LINE_MAX 2048

struct cron_entry {
    unsigned char fields[5][60];
    int dom_star, dow_star;
    char *command;
    struct cron_entry *next;
};
int sched_number(const char *s, unsigned long limit, unsigned long *out);
int sched_directory(const char *path, int create);
int sched_spool(const char *path, int create);
char *sched_path(const char *dir, const char *name);
int sched_quote(FILE *f, const char *text);
int sched_file(const char *path, struct stat *st);
int sched_line(FILE *f, char *line, unsigned size);
int sched_id(const char *id);
int sched_job(const char *path, const char *id, time_t *due);
int sched_cron_load(const char *path, struct cron_entry **head);
void sched_cron_free(struct cron_entry *head);
int sched_cron_match(const struct cron_entry *entry, const struct tm *tm);
int sched_local(struct tm *t, time_t *out);
int sched_time(const char *stamp, time_t *out);
#endif
