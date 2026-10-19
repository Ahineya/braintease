/* SPDX-License-Identifier: MIT */
#ifndef CHAT_COMPOSER_H
#define CHAT_COMPOSER_H
#include "chat.h"
#define CHAT_MATCHES 64
typedef struct {
    char text[CHAT_INPUT + 1], killed[CHAT_INPUT + 1];
    unsigned cursor;
} ChatEditor;
typedef struct { char name[256]; int directory; } ChatMatch;
typedef struct {
    ChatMatch entries[CHAT_MATCHES];
    char parent[512], error[96];
    unsigned start, end, count, selected;
    int active, more;
} ChatMentions;
enum {
    EDIT_LEFT, EDIT_RIGHT, EDIT_WORD_LEFT, EDIT_WORD_RIGHT, EDIT_BACKSPACE,
    EDIT_DELETE, EDIT_WORD_ERASE, EDIT_WORD_DELETE, EDIT_HOME, EDIT_END,
    EDIT_KILL_BEFORE, EDIT_KILL_AFTER, EDIT_YANK
};
int chat_editor_insert(ChatEditor *, const char *, unsigned);
void chat_editor_action(ChatEditor *, unsigned);
void chat_mentions(Chat *, ChatEditor *, ChatMentions *);
int chat_mention_apply(ChatEditor *, ChatMentions *);
#endif
