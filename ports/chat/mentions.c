/* SPDX-License-Identifier: MIT */
/* Complete one directory level; never recursively crawl or preload file data. */
#include "composer.h"
#include "json.h"
#include <dirent.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static int space(unsigned ch) { return ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n'; }
static int mention(ChatEditor *e, ChatMentions *m, char *token) {
    unsigned i, at, n, end, quoted, ch, size = strlen(e->text);
    for (i = 0; i < e->cursor; i++) {
        if (e->text[i] != '@' || (i && !space((unsigned char)e->text[i - 1]) &&
            !strchr("([{", e->text[i - 1]))) continue;
        at = i + 1; n = 0;
        quoted = e->text[at] == '"';
        if (quoted) ++at;
        while (at < e->cursor) {
            ch = (unsigned char)e->text[at++];
            if (quoted ? ch == '"' || ch == '\n' : space(ch) || ch == '"') break;
            if (quoted && ch == '\\' && at < e->cursor) ch = (unsigned char)e->text[at++];
            if (n == 499) return 0;
            token[n++] = ch;
        }
        if (at != e->cursor || (at > i + 1 + quoted &&
            (quoted ? ch == '"' || ch == '\n' : space(ch) || ch == '"'))) continue;
        token[n] = 0;
        end = e->cursor;
        while (end < size) {
            ch = (unsigned char)e->text[end++];
            if (quoted && ch == '\\' && end < size) { ++end; continue; }
            if (quoted && ch == '"') break;
            if (!quoted && space(ch)) { --end; break; }
        }
        m->start = i; m->end = end;
        return 1;
    }
    return 0;
}
static int order(const void *a, const void *b) {
    const ChatMatch *left = a, *right = b;
    if (left->directory != right->directory) return right->directory - left->directory;
    return strcmp(left->name, right->name);
}
void chat_mentions(Chat *c, ChatEditor *e, ChatMentions *m) {
    char token[512], full[1024], child[1024], *slash, *prefix;
    unsigned n, scanned = 0, i;
    struct dirent *entry;
    struct stat st;
    DIR *directory;
    m->active = m->more = m->count = m->selected = 0;
    m->error[0] = 0;
    if (!c->workspace[0] || !mention(e, m, token)) return;
    m->active = 1;
    slash = strrchr(token, '/');
    n = slash ? (unsigned)(slash - token + 1) : 0;
    memcpy(m->parent, token, n); m->parent[n] = 0;
    prefix = token + n;
    if (strlen(c->workspace) + n + 2 >= sizeof(full)) {
        strcpy(m->error, "Path is too long"); return;
    }
    if (token[0] == '/') strcpy(full, m->parent);
    else snprintf(full, sizeof(full), "%s/%s", c->workspace, m->parent);
    directory = opendir(full);
    if (!directory) { strcpy(m->error, "Cannot open this directory"); return; }
    while ((entry = readdir(directory)) != NULL) {
        if (++scanned > 4096) { m->more = 1; break; }
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..") ||
            (entry->d_name[0] == '.' && prefix[0] != '.') ||
            strncmp(entry->d_name, prefix, strlen(prefix)) || strlen(entry->d_name) >= 256 ||
            !cj_utf8(entry->d_name, strlen(entry->d_name))) continue;
        for (i = 0; entry->d_name[i]; i++)
            if ((unsigned char)entry->d_name[i] < 32 || (unsigned char)entry->d_name[i] == 127) break;
        if (entry->d_name[i]) continue;
        if (m->count == CHAT_MATCHES) { m->more = 1; continue; }
        if (strlen(full) + strlen(entry->d_name) + 2 >= sizeof(child)) continue;
        snprintf(child, sizeof(child), "%s/%s", full, entry->d_name);
        strcpy(m->entries[m->count].name, entry->d_name);
        m->entries[m->count++].directory = !stat(child, &st) && S_ISDIR(st.st_mode);
    }
    closedir(directory);
    qsort(m->entries, m->count, sizeof(m->entries[0]), order);
}
int chat_mention_apply(ChatEditor *e, ChatMentions *m) {
    char path[1024], encoded[2056];
    unsigned n = 0, i, old = strlen(e->text), inserted, quoted = 0;
    ChatMatch *match;
    if (!m->active || !m->count || m->selected >= m->count) return 0;
    match = &m->entries[m->selected];
    snprintf(path, sizeof(path), "%s%s%s", m->parent, match->name, match->directory ? "/" : "");
    if (strlen(path) >= 500) return 0;
    for (i = 0; path[i]; i++) if (space((unsigned char)path[i]) || path[i] == '"' || path[i] == '\\') quoted = 1;
    encoded[n++] = '@';
    if (quoted) encoded[n++] = '"';
    for (i = 0; path[i]; i++) {
        if (quoted && (path[i] == '"' || path[i] == '\\')) encoded[n++] = '\\';
        encoded[n++] = path[i];
    }
    inserted = n;
    if (quoted) encoded[n++] = '"';
    if (!match->directory) encoded[n++] = ' ';
    if (old - (m->end - m->start) + n > CHAT_INPUT) return 0;
    memmove(e->text + m->start + n, e->text + m->end, old - m->end + 1);
    memcpy(e->text + m->start, encoded, n);
    e->cursor = m->start + (match->directory ? inserted : n);
    return 1;
}
