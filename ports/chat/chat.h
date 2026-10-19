/* SPDX-License-Identifier: MIT */
#ifndef FUSE_CHAT_H
#define FUSE_CHAT_H
#include <stdio.h>
#include <sys/types.h>
#define CHAT_MODEL "gpt-6.1-sol"
#define CHAT_EFFORT "low"
#ifdef FUSE_X_USER
#define CHAT_SYSTEM "native Fuse-X"
#else
#define CHAT_SYSTEM "Riplix/RVM"
#endif
#define CHAT_MAX 128
#define CHAT_TEXT 131072UL
#define CHAT_ITEM 524288UL
#define CHAT_HISTORY 4194304UL
#define CHAT_INPUT 8192
#define CHAT_TOOL_TEXT 32768UL
#define CHAT_INSTRUCTIONS 16384UL
enum { CHAT_SIDEBAR_AUTO, CHAT_SIDEBAR_HIDDEN, CHAT_SIDEBAR_VISIBLE };

typedef struct {
    char root[256], host[64], status[256];
    char workspace[512];
    unsigned ids[CHAT_MAX], count, selected;
    char titles[CHAT_MAX][96];
    char *display, *reply;
    unsigned long revision, reply_size;
    int lock, pipefd, echo, outcome, sidebar, expand_tools;
    pid_t worker;
    char frame[4100];
    unsigned framed;
} Chat;
int chat_open(Chat *);
void chat_close(Chat *);
int chat_new(Chat *);
int chat_workspace(Chat *, const char *);
int chat_instructions(Chat *, const char *);
int chat_sidebar(Chat *, int);
int chat_select(Chat *, unsigned);
int chat_send(Chat *, const char *);
void chat_step(Chat *);
void chat_cancel(Chat *);
int chat_import(Chat *, const char *);
int chat_login_request(Chat *, const char *);
int chat_probe(Chat *);
void chat_path(Chat *, char *, const char *);
char *chat_read(const char *, unsigned long);
int chat_atomic(const char *, const char *);
int chat_load(Chat *);
int chat_tool_view(Chat *);
int chat_commit(Chat *, const char *);
FILE *chat_request(Chat *);
void chat_worker(Chat *, int);
int chat_ui(Chat *);
int chat_object(FILE *, char **);
void chat_history_path(Chat *, char *);
void chat_message(int, unsigned, const char *);
void chat_tool_definitions(FILE *);
int chat_tools_pending(Chat *, int, int);
void chat_tools_init(void);
int chat_tools_cancelled(void);
char *chat_tool_run(Chat *, const char *, const char *, int);
FILE *chat_temporary(Chat *);
char *chat_file_text(FILE *, unsigned long);
#endif
