/* SPDX-License-Identifier: MIT */
#include "tui_theme.h"
#include "../ini/ini.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#define ROLES 63
#define LABEL 128
struct Style { int fg, bg; unsigned flags; };
struct Role { char *key; struct Style initial; };
struct Values { char name[LABEL]; int lines; struct Style roles[ROLES]; };
struct TuiTheme {
    struct Role roles[ROLES];
    unsigned count;
    struct Values initial, value;
    char error[256];
};
static int fail(TuiTheme *t, const char *message) {
    strncpy(t->error, message, sizeof(t->error) - 1);
    t->error[sizeof(t->error) - 1] = 0;
    return -1;
}
TuiTheme *tui_theme_new(const char *name, int lines) {
    TuiTheme *t;
    if (!name || strlen(name) >= LABEL) return NULL;
    t = calloc(1, sizeof(*t));
    if (t) {
        strcpy(t->initial.name, name); t->initial.lines = !!lines;
        t->value = t->initial;
    }
    return t;
}
void tui_theme_free(TuiTheme *t) {
    unsigned i;
    if (t) { for (i = 0; i < t->count; ++i) free(t->roles[i].key); free(t); }
}
int tui_theme_define(TuiTheme *t, const char *key, int fg, int bg, unsigned flags) {
    char *copy; unsigned i;
    if (!t || !key || !*key) return -1;
    if (t->count == ROLES || fg < -1 || fg > 255 || bg < -1 || bg > 255 || flags > 7)
        return fail(t, "Invalid role or color-pair limit");
    for (i = 0; i < t->count; ++i) if (!strcmp(key, t->roles[i].key)) return fail(t, "Duplicate color role");
    copy = malloc(strlen(key) + 1);
    if (!copy) return fail(t, "Out of memory");
    strcpy(copy, key); i = t->count++;
    t->roles[i].key = copy;
    t->roles[i].initial.fg = fg; t->roles[i].initial.bg = bg; t->roles[i].initial.flags = flags;
    t->initial.roles[i] = t->roles[i].initial; t->value.roles[i] = t->roles[i].initial;
    return 0;
}
static int color(const char *s) {
    static const char *names[] = {"black", "red", "green", "yellow", "blue", "magenta", "cyan", "white"};
    unsigned i; int n = 0, bright = 0;
    if (!strcmp(s, "default")) return -1;
    if (!strncmp(s, "bright-", 7)) { bright = 8; s += 7; }
    for (i = 0; i < 8; ++i) if (!strcmp(s, names[i])) return i + bright;
    if (bright || !*s || strlen(s) > 3) return -2;
    while (*s) { if (*s < '0' || *s > '9') return -2; n = n * 10 + *s++ - '0'; }
    return n <= 255 ? n : -2;
}
static int style(TuiTheme *t, const char *text, struct Style *out) {
    char buffer[1025], *p, *part; int n = 0, c;
    if (strlen(text) > 1024) return fail(t, "Color value exceeds line limit");
    strcpy(buffer, text); p = buffer; out->flags = 0;
    while (*p) {
        while (*p == ',' || *p == ' ' || *p == '\t') ++p;
        if (!*p) break;
        part = p; while (*p && *p != ',' && *p != ' ' && *p != '\t') ++p;
        if (*p) *p++ = 0;
        if (n < 2) {
            c = color(part);
            if (c < -1) return fail(t, "Unknown color (expected name, default or 0..255)");
            if (n == 0) out->fg = c; else out->bg = c;
        } else if (!strcmp(part, "bold")) out->flags |= TUI_BOLD;
        else if (!strcmp(part, "reverse")) out->flags |= TUI_REVERSE;
        else if (!strcmp(part, "underline")) out->flags |= TUI_UNDERLINE;
        else if (strcmp(part, "normal")) return fail(t, "Unknown color attribute");
        ++n;
    }
    return n >= 2 ? 0 : fail(t, "Expected foreground and background");
}
int tui_theme_apply(TuiTheme *t, const char *section, const char *key, const char *value) {
    unsigned i; struct Style s;
    if (!strcmp(section, "theme")) {
        if (!strcmp(key, "base")) return 0; /* Resolved by the file loader first. */
        if (!strcmp(key, "name")) {
            if (strlen(value) >= LABEL) return fail(t, "Theme name exceeds 127 characters");
            strcpy(t->value.name, value); return 0;
        }
        if (!strcmp(key, "borders")) {
            if (strcmp(value, "line") && strcmp(value, "ascii")) return fail(t, "borders must be line or ascii");
            t->value.lines = !strcmp(value, "line"); return 0;
        }
        return fail(t, "Unknown theme option");
    }
    if (strcmp(section, "colors")) return fail(t, "Unknown theme section");
    for (i = 0; i < t->count; ++i) if (!strcmp(key, t->roles[i].key)) {
        if (style(t, value, &s) < 0) return -1;
        t->value.roles[i] = s; return 0;
    }
    return fail(t, "Unknown color role");
}
int tui_theme_valid_name(const char *s) {
    unsigned n = 0;
    if (!s || !*s) return 0;
    while (*s) {
        if (++n > 24 || !((*s >= 'a' && *s <= 'z') || (*s >= 'A' && *s <= 'Z') ||
                          (*s >= '0' && *s <= '9') || *s == '-' || *s == '_')) return 0;
        ++s;
    }
    return 1;
}
/* A bounded regular-file read. Missing optional configuration is not an error. */
static char *read_file(TuiTheme *t, const char *path, int optional, int *missing) {
    struct stat info; FILE *f; char *text; size_t n;
    *missing = 0;
    if (stat(path, &info) < 0) {
        if (errno == ENOENT && optional) { *missing = 1; return NULL; }
        fail(t, strerror(errno)); return NULL;
    }
    if (!S_ISREG(info.st_mode)) { fail(t, "Expected a regular theme file"); return NULL; }
    f = fopen(path, "r");
    if (!f) { fail(t, strerror(errno)); return NULL; }
    text = malloc(16386UL);
    if (!text) { fclose(f); fail(t, "Out of memory"); return NULL; }
    n = fread(text, 1, 16385UL, f);
    if (ferror(f)) { fail(t, strerror(errno)); free(text); fclose(f); return NULL; }
    if (fclose(f)) { fail(t, strerror(errno)); free(text); return NULL; }
    if (n > 16384UL) { fail(t, "Theme exceeds 16 KiB"); free(text); return NULL; }
    /* ini_reader uses an explicit length, so embedded NUL is rejected there. */
    text[n] = 0;
    if (memchr(text, 0, n)) { fail(t, "Invalid INI syntax (NUL)"); free(text); return NULL; }
    return text;
}
static int load(TuiTheme *, const char *, const char *, int, char [8][25], unsigned);
static int named(TuiTheme *t, const char *dir, const char *name, char seen[8][25], unsigned depth) {
    char path[512]; unsigned i;
    if (!tui_theme_valid_name(name)) return fail(t, "Invalid base theme name");
    if (depth >= 8) return fail(t, "Theme inheritance exceeds eight files");
    for (i = 0; i < depth; ++i) if (!strcmp(seen[i], name)) return fail(t, "Theme inheritance cycle");
    if (strlen(dir) + strlen(name) + 6 > sizeof(path)) return fail(t, "Theme path too long");
    strcpy(seen[depth], name);
    strcpy(path, dir); strcat(path, "/"); strcat(path, name); strcat(path, ".ini");
    return load(t, dir, path, 0, seen, depth + 1);
}
static int load(TuiTheme *t, const char *dir, const char *path, int optional, char seen[8][25], unsigned depth) {
    char *text; char base[1025]; IniReader *r; int result, missing, has_base = 0;
    text = read_file(t, path, optional, &missing);
    if (!text) return missing ? 0 : -1;
    r = ini_reader(text, strlen(text));
    if (!r) { free(text); return fail(t, "Out of memory"); }
    while ((result = ini_next(r)) > 0) {
        if (!strcmp(ini_section(r), "theme") && !strcmp(ini_key(r), "base")) {
            strcpy(base, ini_value(r)); has_base = 1;
        }
    }
    ini_free(r);
    if (result < 0) { free(text); return fail(t, "Invalid INI syntax, base name or input limit"); }
    if (has_base) {
        if (!tui_theme_valid_name(base)) { free(text); return fail(t, "Invalid base theme name"); }
        /* A layer with a base starts from that base, not the preceding layer. */
        t->value = t->initial;
        if (named(t, dir, base, seen, depth) < 0) { free(text); return -1; }
    }
    r = ini_reader(text, strlen(text)); free(text);
    if (!r) return fail(t, "Out of memory");
    while ((result = ini_next(r)) > 0) {
        if (tui_theme_apply(t, ini_section(r), ini_key(r), ini_value(r)) < 0) { ini_free(r); return -1; }
    }
    ini_free(r);
    return result < 0 ? fail(t, "Invalid INI syntax or input limit") : 0;
}
int tui_theme_named(TuiTheme *t, const char *dir, const char *name) {
    struct Values old = t->value; char seen[8][25]; int result;
    t->error[0] = 0; t->value = t->initial;
    result = named(t, dir, name, seen, 0);
    if (result < 0) t->value = old;
    return result;
}
int tui_theme_file(TuiTheme *t, const char *dir, const char *path, int optional) {
    struct Values old = t->value; char seen[8][25]; int result;
    t->error[0] = 0;
    result = load(t, dir, path, optional, seen, 0);
    if (result < 0) t->value = old;
    return result;
}
const char *tui_theme_error(const TuiTheme *t) { return t->error; }
const char *tui_theme_name(const TuiTheme *t) { return t->value.name; }
int tui_theme_lines(const TuiTheme *t) { return t->value.lines; }
unsigned tui_theme_count(const TuiTheme *t) { return t->count; }
int tui_theme_fg(const TuiTheme *t, unsigned i) { return i < t->count ? t->value.roles[i].fg : -1; }
int tui_theme_bg(const TuiTheme *t, unsigned i) { return i < t->count ? t->value.roles[i].bg : -1; }
unsigned tui_theme_flags(const TuiTheme *t, unsigned i) { return i < t->count ? t->value.roles[i].flags : 0; }
