/* SPDX-License-Identifier: MIT */
#include "ui-document.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <errno.h>

struct stamp {
    unsigned long hash, length;
    struct stat st;
    int exists;
};
struct ui_document {
    TextBuffer *text;
    char *path;
    char untitled[32];
    struct stamp stamp;
    unsigned long cursor, anchor;
    unsigned language;
};
struct ui_documents {
    ui_document *tabs[UI_DOCUMENT_TABS];
    char *recent[UI_DOCUMENT_RECENTS], *settings;
    unsigned count, active, serial, nrecent;
    int wrap;
    char error[200];
};
static int fail(ui_documents *s, const char *message) {
    snprintf(s->error, sizeof(s->error), "%s", message);
    return UI_DOCUMENT_ERROR;
}
static int io_error(ui_documents *s, const char *operation) {
    snprintf(s->error, sizeof(s->error), "%s: %s", operation, strerror(errno));
    return UI_DOCUMENT_ERROR;
}
static char *copy_string(const char *p) {
    char *q = malloc(strlen(p) + 1);
    if (q) strcpy(q, p);
    return q;
}
static char *absolute_path(ui_documents *s, const char *p) {
    char *q;
    size_t n;
    if (!p || !*p || strlen(p) >= UI_DOCUMENT_PATH) {
        fail(s, "Choose a path shorter than 2048 characters.");
        return NULL;
    }
    q = malloc(UI_DOCUMENT_PATH);
    if (!q) { fail(s, "Not enough memory."); return NULL; }
    if (*p == '/') strcpy(q, p);
    else {
        if (!getcwd(q, UI_DOCUMENT_PATH)) { free(q); io_error(s, "Read working directory"); return NULL; }
        n = strlen(q);
        if (n + strlen(p) + 2 > UI_DOCUMENT_PATH) {
            free(q); fail(s, "The absolute path is too long."); return NULL;
        }
        if (n > 1) q[n++] = '/';
        strcpy(q + n, p);
    }
    return q;
}
static int regular(ui_documents *s, const char *path, struct stat *st, int *exists,
                   int refuse_link) {
    *exists = 1;
    if ((refuse_link ? lstat(path, st) : stat(path, st)) < 0) {
        if (errno == ENOENT) { *exists = 0; return 0; }
        return io_error(s, "Inspect file");
    }
    if (S_ISLNK(st->st_mode))
        return fail(s, "Saving over a symbolic link is not supported. Choose a regular-file path.");
    if (!S_ISREG(st->st_mode)) return fail(s, "Choose a regular text file.");
    return 0;
}
static int same_stamp(const struct stamp *a, const struct stamp *b) {
    return a->exists == b->exists && (!a->exists ||
        (a->st.st_dev == b->st.st_dev && a->st.st_ino == b->st.st_ino &&
         a->st.st_mtime == b->st.st_mtime && a->st.st_mode == b->st.st_mode &&
         a->length == b->length && a->hash == b->hash));
}
/* Always read actual content for a fingerprint. Optional buffer is private to
 * the caller until this function succeeds, including close and fstat checks. */
static int read_file(ui_documents *s, const char *path, struct stamp *stamp,
                     TextBuffer *buffer, int missing_ok) {
    unsigned char *chunk = malloc(4096);
    struct stat before, after;
    unsigned long total = 0, hash = 2166136261UL;
    int fd, exists, bad = 0;
    ssize_t n;
    memset(stamp, 0, sizeof(*stamp));
    if (!chunk) return fail(s, "Not enough memory.");
    if (regular(s, path, &before, &exists, 0)) { free(chunk); return -1; }
    if (!exists) {
        free(chunk);
        return missing_ok ? 0 : fail(s, "The file no longer exists.");
    }
    if (before.st_size < 0 || (unsigned long)before.st_size > TEXT_MAX) {
        free(chunk); return fail(s, "Files must fit within 512 KiB.");
    }
    fd = open(path, O_RDONLY);
    if (fd < 0) { free(chunk); return io_error(s, "Open file"); }
    if (fstat(fd, &before) < 0 || !S_ISREG(before.st_mode)) {
        close(fd); free(chunk); return fail(s, "Cannot inspect the opened regular file.");
    }
    while ((n = read(fd, chunk, 4096)) != 0) {
        if (n < 0) {
            if (errno == EINTR) continue;
            io_error(s, "Read file"); bad = 1; break;
        }
        if ((unsigned long)n > TEXT_MAX - total) {
            fail(s, "Files must fit within 512 KiB."); bad = 1; break;
        }
        total += (unsigned long)n;
        hash = text_hash(chunk, (unsigned long)n, hash);
        if (buffer && text_append(buffer, chunk, (unsigned long)n)) {
            fail(s, text_error(buffer)); bad = 1; break;
        }
    }
    if (!bad && (fstat(fd, &after) < 0 || before.st_dev != after.st_dev ||
        before.st_ino != after.st_ino || before.st_size != after.st_size ||
        before.st_mtime != after.st_mtime || (unsigned long)after.st_size != total)) {
        fail(s, "The file changed while it was being read. Try again."); bad = 1;
    }
    if (close(fd) && !bad) { io_error(s, "Close file"); bad = 1; }
    free(chunk);
    if (bad) return -1;
    stamp->exists = 1; stamp->st = after; stamp->length = total; stamp->hash = hash;
    return 0;
}
static const struct ui_document_language languages[] = {
    {UI_LANGUAGE_AUTOMATIC, 0, "Automatic"},
    {UI_LANGUAGE_TEXT, 0, "Plain text"},
    {UI_LANGUAGE_C, 1, "C"},
    {UI_LANGUAGE_CPP, 1, "C++"},
    {UI_LANGUAGE_RFL, 1, "RFL"},
    {UI_LANGUAGE_RUST, 1, "Rust"},
    {UI_LANGUAGE_JAVA, 1, "Java"},
    {UI_LANGUAGE_SHELL, 2, "Shell"},
    {UI_LANGUAGE_TCL, 2, "Tcl"},
    {UI_LANGUAGE_AWK, 2, "Awk"},
    {UI_LANGUAGE_PYTHON, 3, "Python"},
    {UI_LANGUAGE_PASCAL, 4, "Pascal"},
    {UI_LANGUAGE_JSON, 5, "JSON"},
    {UI_LANGUAGE_INI, 6, "INI"},
    {UI_LANGUAGE_MARKDOWN, 7, "Markdown"},
    {UI_LANGUAGE_BRAINFUCK, 8, "Brainfuck"},
    {UI_LANGUAGE_MAKEFILE, 9, "Makefile"},
    {UI_LANGUAGE_HTML, 10, "HTML"},
    {UI_LANGUAGE_CSS, 10, "CSS"},
    {UI_LANGUAGE_LISP, 11, "Lisp"},
    {UI_LANGUAGE_SCHEME, 11, "Scheme"},
    {UI_LANGUAGE_ASSEMBLY, 12, "Assembly"},
    {UI_LANGUAGE_BASIC, 13, "BASIC"},
    {UI_LANGUAGE_JAVASCRIPT, 14, "JavaScript"},
    {UI_LANGUAGE_SYMA, 15, "Syma"}
};
unsigned ui_document_language_count(void) { return sizeof(languages)/sizeof(languages[0]); }
const struct ui_document_language *ui_document_language_at(unsigned index) {
    return index < ui_document_language_count() ? &languages[index] : NULL;
}
static unsigned language_for_path(const char *path) {
    const char *p = strrchr(path, '.');
    if (!strcmp(path + (strrchr(path, '/') ? strrchr(path, '/') - path + 1 : 0), "Makefile")) return UI_LANGUAGE_MAKEFILE;
    if (!p) return UI_LANGUAGE_TEXT;
    if (!strcmp(p, ".syma") || !strcmp(p, ".sym")) return UI_LANGUAGE_SYMA;
    if (!strcmp(p, ".js") || !strcmp(p, ".mjs") || !strcmp(p, ".cjs")) return UI_LANGUAGE_JAVASCRIPT;
    if (!strcmp(p, ".c") || !strcmp(p, ".h")) return UI_LANGUAGE_C;
    if (!strcmp(p, ".cpp") || !strcmp(p, ".cc") || !strcmp(p, ".cxx") || !strcmp(p, ".hpp")) return UI_LANGUAGE_CPP;
    if (!strcmp(p, ".rfl")) return UI_LANGUAGE_RFL;
    if (!strcmp(p, ".rs")) return UI_LANGUAGE_RUST;
    if (!strcmp(p, ".java")) return UI_LANGUAGE_JAVA;
    if (!strcmp(p, ".sh")) return UI_LANGUAGE_SHELL;
    if (!strcmp(p, ".tcl")) return UI_LANGUAGE_TCL;
    if (!strcmp(p, ".awk")) return UI_LANGUAGE_AWK;
    if (!strcmp(p, ".py")) return UI_LANGUAGE_PYTHON;
    if (!strcmp(p, ".pas")) return UI_LANGUAGE_PASCAL;
    if (!strcmp(p, ".json")) return UI_LANGUAGE_JSON;
    if (!strcmp(p, ".ini") || !strcmp(p, ".conf")) return UI_LANGUAGE_INI;
    if (!strcmp(p, ".md")) return UI_LANGUAGE_MARKDOWN;
    if (!strcmp(p, ".bf")) return UI_LANGUAGE_BRAINFUCK;
    if (!strcmp(p, ".html") || !strcmp(p, ".htm")) return UI_LANGUAGE_HTML;
    if (!strcmp(p, ".css")) return UI_LANGUAGE_CSS;
    if (!strcmp(p, ".lisp")) return UI_LANGUAGE_LISP;
    if (!strcmp(p, ".scm")) return UI_LANGUAGE_SCHEME;
    if (!strcmp(p, ".s") || !strcmp(p, ".asm")) return UI_LANGUAGE_ASSEMBLY;
    if (!strcmp(p, ".bas")) return UI_LANGUAGE_BASIC;
    return UI_LANGUAGE_TEXT;
}
unsigned ui_document_get_language(ui_document *d) { return d ? d->language : UI_LANGUAGE_AUTOMATIC; }
unsigned ui_document_effective_language(ui_document *d) {
    return d && d->language ? d->language : language_for_path(d && d->path ? d->path : "");
}
const char *ui_document_language_name(ui_document *d) { return languages[ui_document_effective_language(d)].name; }
static void apply_language(ui_document *d) { text_language(d->text,languages[ui_document_effective_language(d)].lexer); }
int ui_document_set_language(ui_document *d, unsigned language) {
    if (!d || language >= ui_document_language_count()) return -1;
    d->language = language; apply_language(d); return 0;
}
static void recent_add(ui_documents *s, const char *path) {
    unsigned i, j;
    char *p;
    for (i = 0; i < s->nrecent; i++) if (!strcmp(s->recent[i], path)) break;
    if (i < s->nrecent) p = s->recent[i];
    else {
        p = copy_string(path);
        if (!p) return;
        if (s->nrecent < UI_DOCUMENT_RECENTS) s->nrecent++;
        else free(s->recent[UI_DOCUMENT_RECENTS - 1]);
        i = s->nrecent - 1;
    }
    for (j = i; j; j--) s->recent[j] = s->recent[j - 1];
    s->recent[0] = p;
}
static unsigned unhex(unsigned c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return 16;
}
static void settings_load(ui_documents *s) {
    FILE *f;
    char *line, *decoded;
    struct stat st;
    unsigned count = 0;
    if (!s->settings || lstat(s->settings, &st) || !S_ISREG(st.st_mode) ||
        st.st_size > 65536 || st.st_size < 0) return;
    line = malloc(UI_DOCUMENT_PATH * 2 + 16);
    decoded = malloc(UI_DOCUMENT_PATH);
    if (!line || !decoded) { free(line); free(decoded); return; }
    f = fopen(s->settings, "r");
    if (!f) { free(line); free(decoded); return; }
    while (fgets(line, UI_DOCUMENT_PATH * 2 + 16, f) && count++ < 64) {
        char *end = strchr(line, '\n');
        unsigned i, n;
        if (!end && !feof(f)) break;
        if (end) *end = 0;
        if (!strcmp(line, "wrap=0")) s->wrap = 0;
        else if (!strcmp(line, "wrap=1")) s->wrap = 1;
        else if (!strncmp(line, "recent=", 7) && s->nrecent < UI_DOCUMENT_RECENTS) {
            n = strlen(line + 7);
            if (!n || (n & 1) || n / 2 >= UI_DOCUMENT_PATH) continue;
            for (i = 0; i < n / 2; i++) {
                unsigned a = unhex((unsigned char)line[7 + i * 2]);
                unsigned b = unhex((unsigned char)line[8 + i * 2]);
                if (a == 16 || b == 16 || !(a * 16 + b)) break;
                decoded[i] = a * 16 + b;
            }
            decoded[i] = 0;
            if (i == n / 2 && decoded[0] == '/') {
                unsigned j;
                for (j = 0; j < s->nrecent; j++) if (!strcmp(s->recent[j], decoded)) break;
                if (j == s->nrecent) {
                    char *p = copy_string(decoded);
                    if (p) s->recent[s->nrecent++] = p;
                }
            }
        }
    }
    fclose(f); free(line); free(decoded);
}
ui_documents *ui_documents_create(const char *home) {
    ui_documents *s = calloc(1, sizeof(*s));
    if (!s) return NULL;
    s->wrap = 1;
    if (home && home[0] == '/' && strlen(home) + 10 < UI_DOCUMENT_PATH) {
        s->settings = malloc(strlen(home) + 10);
        if (s->settings) {
            snprintf(s->settings, strlen(home) + 10, "%s/.lvedit", home);
            settings_load(s);
        }
    }
    return s;
}
void ui_documents_delete(ui_documents *s) {
    unsigned i;
    if (!s) return;
    for (i = 0; i < s->count; i++) {
        text_free(s->tabs[i]->text); free(s->tabs[i]->path); free(s->tabs[i]);
    }
    for (i = 0; i < s->nrecent; i++) free(s->recent[i]);
    free(s->settings); free(s);
}
const char *ui_documents_error(ui_documents *s) { return s ? s->error : "Not enough memory."; }
unsigned ui_documents_count(ui_documents *s) { return s ? s->count : 0; }
ui_document *ui_documents_at(ui_documents *s, unsigned index) { return s && index < s->count ? s->tabs[index] : NULL; }
ui_document *ui_documents_active(ui_documents *s) { return s && s->count ? s->tabs[s->active] : NULL; }
int ui_documents_select(ui_documents *s, ui_document *d) {
    unsigned i;
    for (i = 0; i < s->count; i++) if (s->tabs[i] == d) { s->active = i; return 0; }
    return fail(s, "The document is no longer open.");
}
ui_document *ui_documents_new(ui_documents *s) {
    ui_document *d;
    if (s->count == UI_DOCUMENT_TABS) { fail(s, "Close a tab before opening another document."); return NULL; }
    d = calloc(1, sizeof(*d));
    if (!d || !(d->text = text_new())) { free(d); fail(s, "Not enough memory."); return NULL; }
    snprintf(d->untitled, sizeof(d->untitled), "Untitled %u", ++s->serial);
    s->active = s->count; s->tabs[s->count++] = d;
    return d;
}
ui_document *ui_documents_open(ui_documents *s, const char *path) {
    char *p = absolute_path(s, path);
    ui_document *d;
    TextBuffer *b;
    struct stamp stamp;
    struct stat st;
    unsigned i;
    int exists;
    if (!p) return NULL;
    if (regular(s, p, &st, &exists, 0)) { free(p); return NULL; }
    for (i = 0; i < s->count; i++) if (s->tabs[i]->path &&
        (!strcmp(s->tabs[i]->path, p) || (exists && s->tabs[i]->stamp.exists &&
         st.st_dev == s->tabs[i]->stamp.st.st_dev && st.st_ino == s->tabs[i]->stamp.st.st_ino))) {
        s->active = i; free(p); return s->tabs[i];
    }
    if (s->count == UI_DOCUMENT_TABS) { free(p); fail(s, "Close a tab before opening another document."); return NULL; }
    b = text_new();
    if (!b) { free(p); fail(s, "Not enough memory."); return NULL; }
    if (read_file(s, p, &stamp, b, 0)) { free(p); text_free(b); return NULL; }
    d = calloc(1, sizeof(*d));
    if (!d) { free(p); text_free(b); fail(s, "Not enough memory."); return NULL; }
    d->text = b; d->path = p; d->stamp = stamp;
    apply_language(d); text_saved(b);
    s->active = s->count; s->tabs[s->count++] = d; recent_add(s, p);
    return d;
}
int ui_documents_close(ui_documents *s, ui_document *d, int discard) {
    unsigned i;
    for (i = 0; i < s->count; i++) if (s->tabs[i] == d) break;
    if (i == s->count) return fail(s, "The document is no longer open.");
    if (!discard && text_modified(d->text)) return UI_DOCUMENT_DIRTY;
    text_free(d->text); free(d->path); free(d);
    memmove(s->tabs + i, s->tabs + i + 1, (s->count - i - 1) * sizeof(*s->tabs));
    s->count--;
    if (s->active > i) s->active--;
    if (s->active >= s->count) s->active = s->count ? s->count - 1 : 0;
    return 0;
}
TextBuffer *ui_document_text(ui_document *d) { return d ? d->text : NULL; }
const char *ui_document_path(ui_document *d) { return d && d->path ? d->path : ""; }
const char *ui_document_name(ui_document *d) {
    const char *last;
    if (!d) return "";
    if (!d->path) return d->untitled;
    last = strrchr(d->path, '/'); return last ? last + 1 : d->path;
}
void ui_document_view(ui_document *d, unsigned long cursor, unsigned long anchor) { d->cursor = cursor; d->anchor = anchor; }
void ui_document_get_view(ui_document *d, unsigned long *cursor, unsigned long *anchor) { *cursor = d->cursor; *anchor = d->anchor; }
int ui_document_external(ui_documents *s, ui_document *d) {
    struct stamp now;
    if (!d || !d->path) return 0;
    if (read_file(s, d->path, &now, NULL, 1)) return -1;
    return !same_stamp(&d->stamp, &now);
}
static int write_all(ui_documents *s, int fd, const unsigned char *p, unsigned long n) {
    while (n) {
        ssize_t done = write(fd, p, n > 4096 ? 4096 : n);
        if (done < 0 && errno == EINTR) continue;
        if (done <= 0) return io_error(s, "Write file");
        p += done; n -= done;
    }
    return 0;
}
int ui_document_save(ui_documents *s, ui_document *d, const char *path, unsigned flags) {
    char *p, *temporary;
    unsigned char *chunk;
    struct stamp current, final, saved;
    struct stat st;
    int exists, original, fd, bad = 0;
    unsigned long offset = 0, length = text_length(d->text), hash = 2166136261UL;
    size_t directory;
    unsigned i;
    if (!path || !*path) path = d->path;
    p = absolute_path(s, path);
    if (!p) return -1;
    original = d->path && !strcmp(d->path, p);
    for (i = 0; i < s->count; i++) if (s->tabs[i] != d && s->tabs[i]->path &&
        !strcmp(s->tabs[i]->path, p)) { free(p); return fail(s, "That path is already open in another tab."); }
    if (regular(s, p, &st, &exists, 1)) { free(p); return -1; }
    if (exists && !original && !(flags & UI_DOCUMENT_REPLACE)) { free(p); return UI_DOCUMENT_EXISTS; }
    if (read_file(s, p, &current, NULL, 1)) { free(p); return -1; }
    if (original && !(flags & UI_DOCUMENT_FORCE) && !same_stamp(&d->stamp, &current)) {
        free(p); return UI_DOCUMENT_CHANGED;
    }
    /* FUZIX directory entries permit 30 octets. Never append a temporary
     * suffix to the destination basename: a valid 30-octet name must save. */
    directory = (size_t)(strrchr(p, '/') - p) + 1;
    if (directory + 15 > UI_DOCUMENT_PATH) { free(p); return fail(s, "The directory path is too long for a safe temporary file."); }
    temporary = malloc(directory + 15); chunk = malloc(4096);
    if (!temporary || !chunk) { free(p); free(temporary); free(chunk); return fail(s, "Not enough memory."); }
    memcpy(temporary,p,directory);
    strcpy(temporary + directory,".lvedit-XXXXXX");
    fd = mkstemp(temporary);
    if (fd < 0) { free(p); free(temporary); free(chunk); return io_error(s, "Create temporary file"); }
    if (exists && fchmod(fd, st.st_mode & 0777) < 0) { io_error(s, "Preserve file permissions"); bad = 1; }
    while (!bad && offset < length) {
        unsigned long n = length - offset;
        if (n > 4096) n = 4096;
        if (text_copy(d->text, offset, n, chunk) || write_all(s, fd, chunk, n)) { bad = 1; break; }
        hash = text_hash(chunk, n, hash); offset += n;
    }
    if (!bad && fstat(fd, &saved.st) < 0) { io_error(s, "Inspect temporary file"); bad = 1; }
    if (close(fd) && !bad) { io_error(s, "Close temporary file"); bad = 1; }
    /* Recheck destination after writing. FORCE grants this explicit save intent,
     * but even it must not silently overwrite a change made during the write. */
    if (!bad && regular(s, p, &st, &exists, 1)) bad = 1;
    if (!bad && read_file(s, p, &final, NULL, 1)) bad = 1;
    if (!bad && !same_stamp(&current, &final)) {
        unlink(temporary); free(p); free(temporary); free(chunk); return UI_DOCUMENT_CHANGED;
    }
    if (!bad && rename(temporary, p)) { io_error(s, "Replace file"); bad = 1; }
    if (bad) unlink(temporary);
    free(temporary); free(chunk);
    if (bad) { free(p); return -1; }
    saved.exists = 1; saved.length = length; saved.hash = hash;
    free(d->path); d->path = p; d->stamp = saved;
    apply_language(d); text_saved(d->text); recent_add(s, p);
    return 0;
}
int ui_document_reload(ui_documents *s, ui_document *d, int discard) {
    TextBuffer *b;
    struct stamp stamp;
    if (!d || !d->path) return fail(s, "Save the document before reloading it.");
    if (text_modified(d->text) && !discard) return UI_DOCUMENT_DIRTY;
    b = text_new();
    if (!b) return fail(s, "Not enough memory.");
    if (read_file(s, d->path, &stamp, b, 0)) { text_free(b); return -1; }
    text_language(b,languages[ui_document_effective_language(d)].lexer); text_saved(b);
    text_free(d->text); d->text = b; d->stamp = stamp;
    if (d->cursor > text_length(b)) d->cursor = text_length(b);
    if (d->anchor > text_length(b)) d->anchor = text_length(b);
    return 0;
}
unsigned ui_documents_recent_count(ui_documents *s) { return s->nrecent; }
const char *ui_documents_recent(ui_documents *s, unsigned index) { return index < s->nrecent ? s->recent[index] : ""; }
void ui_documents_clear_recent(ui_documents *s) { unsigned i; for (i = 0; i < s->nrecent; i++) free(s->recent[i]); s->nrecent = 0; }
int ui_documents_wrap(ui_documents *s) { return s->wrap; }
void ui_documents_set_wrap(ui_documents *s, int wrap) { s->wrap = !!wrap; }
int ui_documents_settings_save(ui_documents *s) {
    char *temporary;
    FILE *f;
    struct stat st;
    unsigned i;
    int fd, bad = 0;
    if (!s->settings) return 0;
    if (!lstat(s->settings, &st) && !S_ISREG(st.st_mode))
        return fail(s, "Settings path must be a regular file.");
    temporary = malloc(strlen(s->settings) + 20);
    if (!temporary) return fail(s, "Not enough memory.");
    snprintf(temporary, strlen(s->settings) + 20, "%s.XXXXXX", s->settings);
    fd = mkstemp(temporary);
    if (fd < 0) { free(temporary); return io_error(s, "Create settings"); }
    f = fdopen(fd, "w");
    if (!f) { close(fd); unlink(temporary); free(temporary); return io_error(s, "Open settings stream"); }
    fprintf(f, "version=1\nwrap=%d\n", s->wrap);
    for (i = 0; i < s->nrecent; i++) {
        const unsigned char *p = (const unsigned char *)s->recent[i];
        fputs("recent=", f);
        while (*p) fprintf(f, "%02x", (unsigned)*p++);
        fputc('\n', f);
    }
    if (ferror(f)) bad = 1;
    if (fclose(f)) bad = 1;
    if (!bad && rename(temporary, s->settings)) bad = 1;
    if (bad) { io_error(s, "Save settings"); unlink(temporary); }
    free(temporary); return bad ? -1 : 0;
}
