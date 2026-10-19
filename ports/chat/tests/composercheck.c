/* SPDX-License-Identifier: MIT */
#include "../composer.h"
#include "../json.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
static void draft(ChatEditor *e, const char *s) {
    strcpy(e->text, s); e->cursor = strlen(s);
}
static void file(const char *root, const char *name) {
    char path[1024]; FILE *f;
    snprintf(path, sizeof(path), "%s/%s", root, name);
    f = fopen(path, "w"); assert(f); fclose(f);
}
int main(int argc, char **argv) {
    Chat c = {0};
    ChatEditor *e = calloc(1, sizeof(*e));
    ChatMentions *m = calloc(1, sizeof(*m));
    char path[1024], name[64];
    unsigned i;
    assert(argc == 2 && e && m);
    strcpy(c.workspace, argv[1]);
    draft(e, "hello world   ");
    chat_editor_action(e, EDIT_WORD_ERASE); assert(!strcmp(e->text, "hello "));
    chat_editor_action(e, EDIT_YANK); assert(!strcmp(e->text, "hello world   "));
    draft(e, "alpha beta tail"); e->cursor = 10;
    chat_editor_action(e, EDIT_WORD_ERASE); assert(!strcmp(e->text, "alpha  tail"));
    chat_editor_action(e, EDIT_YANK); assert(!strcmp(e->text, "alpha beta tail"));
    draft(e, "one/two.c");
    chat_editor_action(e, EDIT_WORD_LEFT); assert(e->cursor == 8);
    chat_editor_action(e, EDIT_WORD_LEFT); assert(e->cursor == 4);
    chat_editor_action(e, EDIT_WORD_RIGHT); assert(e->cursor == 7);
    chat_editor_action(e, EDIT_WORD_DELETE); assert(!strcmp(e->text, "one/two"));
    draft(e, "Ж e\xcc\x81👩‍💻");
    chat_editor_action(e, EDIT_BACKSPACE); assert(!strcmp(e->text, "Ж e\xcc\x81"));
    chat_editor_action(e, EDIT_BACKSPACE); assert(!strcmp(e->text, "Ж "));
    chat_editor_action(e, EDIT_WORD_ERASE); assert(!*e->text && cj_utf8(e->killed, strlen(e->killed)));
    draft(e, "first\nsecond line"); e->cursor = 9;
    chat_editor_action(e, EDIT_HOME); assert(e->cursor == 6);
    chat_editor_action(e, EDIT_END); assert(e->cursor == 17);
    e->cursor = 5; chat_editor_action(e, EDIT_KILL_AFTER); assert(!strcmp(e->text, "firstsecond line"));
    chat_editor_action(e, EDIT_YANK); assert(!strcmp(e->text, "first\nsecond line"));
    e->cursor = 9; chat_editor_action(e, EDIT_KILL_BEFORE); assert(!strcmp(e->text, "ond line"));
    chat_editor_action(e, EDIT_YANK); assert(!strcmp(e->text, "first\nsecond line"));
    memset(e->text, 'x', CHAT_INPUT); e->text[CHAT_INPUT] = 0; e->cursor = 1;
    assert(!chat_editor_insert(e, "y", 1) && strlen(e->text) == CHAT_INPUT);
    snprintf(path, sizeof(path), "%s/folder name", c.workspace); assert(!mkdir(path, 0700));
    file(path, "quote\"name.txt"); file(path, "Ж.txt");
    file(c.workspace, "reference.txt"); file(c.workspace, ".secret");
    draft(e, "See @"); chat_mentions(&c, e, m);
    assert(m->active && m->count == 2 && m->entries[0].directory && !strcmp(m->entries[0].name, "folder name"));
    assert(chat_mention_apply(e, m) && !strcmp(e->text, "See @\"folder name/\""));
    assert(e->text[e->cursor] == '"');
    chat_mentions(&c, e, m); assert(m->count == 2);
    assert(chat_mention_apply(e, m) && !strcmp(e->text, "See @\"folder name/quote\\\"name.txt\" "));
    chat_mentions(&c, e, m); assert(!m->active);
    draft(e, "Read @ref old words"); e->cursor = 9;
    chat_mentions(&c, e, m); assert(m->count == 1 && chat_mention_apply(e, m));
    assert(!strcmp(e->text, "Read @reference.txt  old words"));
    draft(e, "user@example.com"); chat_mentions(&c, e, m); assert(!m->active);
    draft(e, "@."); chat_mentions(&c, e, m); assert(m->count == 1 && !strcmp(m->entries[0].name, ".secret"));
    draft(e, "@no-directory/"); chat_mentions(&c, e, m); assert(m->active && !m->count && m->error[0]);
    draft(e, "@\"folder name/Ж"); chat_mentions(&c, e, m);
    assert(m->count == 1 && chat_mention_apply(e, m) && strstr(e->text, "Ж.txt\" "));
    draft(e, "@"); c.workspace[0] = 0; chat_mentions(&c, e, m); assert(!m->active);
    strcpy(c.workspace, argv[1]);
    for (i = 0; i < 80; i++) { snprintf(name, sizeof(name), "many-%02u", i); file(c.workspace, name); }
    draft(e, "@many-"); chat_mentions(&c, e, m); assert(m->count == CHAT_MATCHES && m->more);
    free(e); free(m);
    puts("Composer and mentions passed: Unicode editing, word/line movement, kill/yank, path quoting, nesting, middle edits, hidden files and bounded completion");
    return 0;
}
