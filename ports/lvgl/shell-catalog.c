/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "shell-catalog.h"
#include "../../libraries/ini/ini.h"
#include <sys/stat.h>
#include <dirent.h>
#include <unistd.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CATALOG_INPUT_LIMIT 1048576UL
#define CATALOG_LINE_LIMIT 4096UL
typedef struct ShellPin { char *id; int value; } ShellPin;
typedef struct ShellChild {
    pid_t pid;
    int fd, announced, reaped, status;
    unsigned received;
    int exec_error;
    char app_id[64];
} ShellChild;
struct ShellCatalog {
    ShellApp *apps;
    size_t count;
    ShellPin *pins;
    size_t pin_count;
    ShellChild *children;
    size_t child_count;
    char *terminal, *terminal_fallback;
    char error[256];
};
static char *copy(const char *s) {
    size_t n = strlen(s) + 1;
    char *p = malloc(n);
    if (p) memcpy(p, s, n);
    return p;
}
static int fail(ShellCatalog *c, const char *message) {
    if (c) snprintf(c->error, sizeof(c->error), "%s", message);
    return -1;
}
static int valid_id(const char *id) {
    size_t i, n;
    if (!id || !(n = strlen(id)) || n >= 64) return 0;
    for (i = 0; i < n; i++) {
        unsigned ch = (unsigned char)id[i];
        if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
              (ch >= '0' && ch <= '9') || ch == '.' || ch == '_' || ch == '-')) return 0;
    }
    return 1;
}
static int valid_text(const char *s, size_t limit, int empty) {
    const unsigned char *p = (const unsigned char *)s;
    size_t n = strlen(s);
    if (n >= limit || (!empty && !n)) return 0;
    while (*p) {
        unsigned long scalar, minimum;
        unsigned following;
        unsigned ch = *p++;
        if (ch < 0x20 || ch == 0x7f) return 0;
        if (ch < 0x80) continue;
        if (ch >= 0xc2 && ch <= 0xdf) { scalar = ch & 31; following = 1; minimum = 0x80; }
        else if (ch >= 0xe0 && ch <= 0xef) { scalar = ch & 15; following = 2; minimum = 0x800; }
        else if (ch >= 0xf0 && ch <= 0xf4) { scalar = ch & 7; following = 3; minimum = 0x10000UL; }
        else return 0;
        while (following--) {
            ch = *p++;
            if ((ch & 0xc0) != 0x80) return 0;
            scalar = scalar * 64 + (ch & 63);
        }
        if (scalar < minimum || scalar > 0x10ffffUL || (scalar >= 0xd800 && scalar <= 0xdfff)) return 0;
    }
    return 1;
}
static int valid_path(const char *s, int empty) {
    return valid_text(s, 1024, empty) && ((!*s && empty) || *s == '/');
}
static int lower(int ch) { return ch >= 'A' && ch <= 'Z' ? ch + ('a' - 'A') : ch; }
static int compare_fold(const char *a, const char *b) {
    while (*a && lower((unsigned char)*a) == lower((unsigned char)*b)) { a++; b++; }
    return lower((unsigned char)*a) - lower((unsigned char)*b);
}
static int compare_app(const void *a, const void *b) {
    const ShellApp *aa = a, *bb = b;
    int result = compare_fold(aa->category, bb->category);
    if (!result) result = compare_fold(aa->name, bb->name);
    return result ? result : strcmp(aa->id, bb->id);
}
static int contains(const char *text, const char *word, size_t length) {
    while (*text) {
        size_t i = 0;
        while (i < length && text[i] && lower((unsigned char)text[i]) == lower((unsigned char)word[i])) i++;
        if (i == length) return 1;
        text++;
    }
    return !length;
}
static void app_free(ShellApp *app) {
    size_t i;
    free(app->id); free(app->name); free(app->icon); free(app->category); free(app->keywords);
    free(app->executable); free(app->fallback);
    for (i = 0; i < app->argument_count; i++) free(app->arguments[i]);
    free(app->arguments);
    memset(app, 0, sizeof(*app));
}
ShellCatalog *shell_catalog_new(void) {
    ShellCatalog *c = calloc(1, sizeof(*c));
    if (c && shell_catalog_terminal(c, "/bin/gterm", "") < 0) { shell_catalog_free(c); return NULL; }
    return c;
}
void shell_catalog_free(ShellCatalog *c) {
    size_t i;
    if (!c) return;
    for (i = 0; i < c->count; i++) app_free(c->apps + i);
    for (i = 0; i < c->pin_count; i++) free(c->pins[i].id);
    for (i = 0; i < c->child_count; i++) if (c->children[i].fd >= 0) close(c->children[i].fd);
    free(c->apps); free(c->pins); free(c->children);
    free(c->terminal); free(c->terminal_fallback); free(c);
}
const char *shell_catalog_error(const ShellCatalog *c) { return c ? c->error : "No catalog"; }
size_t shell_catalog_count(const ShellCatalog *c) { return c ? c->count : 0; }
const ShellApp *shell_catalog_at(const ShellCatalog *c, size_t index) { return c && index < c->count ? c->apps + index : NULL; }
const ShellApp *shell_catalog_find(const ShellCatalog *c, const char *id) {
    size_t i;
    if (c && id) for (i = 0; i < c->count; i++) if (!strcmp(c->apps[i].id, id)) return c->apps + i;
    return NULL;
}
int shell_app_matches(const ShellApp *app, const char *query) {
    if (!app || !query) return 0;
    while (*query) {
        const char *start;
        size_t length;
        while (*query == ' ' || *query == '\t') query++;
        start = query;
        while (*query && *query != ' ' && *query != '\t') query++;
        length = query - start;
        if (length && !contains(app->name, start, length) && !contains(app->category, start, length) &&
            !contains(app->keywords, start, length) && !contains(app->id, start, length)) return 0;
    }
    return 1;
}
static const char *available(const char *installed, const char *fallback) {
    struct stat st;
    if (installed && *installed && !stat(installed, &st) && S_ISREG(st.st_mode) && !access(installed, X_OK)) return installed;
    if (fallback && *fallback && !stat(fallback, &st) && S_ISREG(st.st_mode) && !access(fallback, X_OK)) return fallback;
    return NULL;
}
const char *shell_app_available(const ShellCatalog *c, const ShellApp *app) {
    if (!c || !app) return NULL;
    if (app->kind == SHELL_APP_TERMINAL && !available(c->terminal, c->terminal_fallback)) return NULL;
    return available(app->executable, app->fallback);
}
int shell_catalog_terminal(ShellCatalog *c, const char *installed, const char *fallback) {
    char *first, *second;
    if (!c || !installed || !fallback || !valid_path(installed, 0) || !valid_path(fallback, 1)) { errno = EINVAL; return fail(c, "Invalid terminal executable path"); }
    first = copy(installed); second = copy(fallback);
    if (!first || !second) { free(first); free(second); return fail(c, "Out of memory"); }
    free(c->terminal); free(c->terminal_fallback); c->terminal = first; c->terminal_fallback = second;
    return 0;
}
static char *read_file(ShellCatalog *c, const char *path, size_t *length) {
    FILE *f = fopen(path, "r");
    char *data = NULL;
    size_t capacity = 0, used = 0;
    if (!f) { fail(c, "Cannot open configuration file"); return NULL; }
    for (;;) {
        size_t got;
        if (used == capacity) {
            size_t next = capacity ? capacity * 2 : 4096;
            char *p;
            if (next > CATALOG_INPUT_LIMIT) next = CATALOG_INPUT_LIMIT;
            if (next == capacity) {
                int extra = fgetc(f);
                if (extra == EOF && !ferror(f)) { data[used] = 0; *length = used; fclose(f); return data; }
                errno = EFBIG; fail(c, "Configuration exceeds 1 Mi-cell input budget"); break;
            }
            p = realloc(data, next + 1);
            if (!p) { fail(c, "Out of memory"); break; }
            data = p; capacity = next;
        }
        got = fread(data + used, 1, capacity - used, f); used += got;
        if (ferror(f)) { fail(c, "Cannot read configuration file"); break; }
        if (feof(f)) { data[used] = 0; *length = used; fclose(f); return data; }
    }
    fclose(f); free(data); return NULL;
}
static int assign(char **field, const char *value) {
    if (*field) return -1;
    *field = copy(value);
    return *field ? 0 : -1;
}
typedef struct Argument { unsigned long index; char *value; } Argument;
static int compare_arg(const void *a, const void *b) {
    unsigned long aa = ((const Argument *)a)->index, bb = ((const Argument *)b)->index;
    return aa < bb ? -1 : aa != bb;
}
static int boolean(const char *s) { return !strcmp(s, "true") ? 1 : !strcmp(s, "false") ? 0 : -1; }
static int geometry_value(const char *s, unsigned max, unsigned *out) {
    unsigned long value = 0;
    if (!*s) return -1;
    while (*s >= '0' && *s <= '9') { value = value * 10 + *s++ - '0'; if (value > max) return -1; }
    if (*s || !value) return -1;
    *out = (unsigned)value; return 0;
}
static int parse_app(ShellCatalog *c, const char *path, ShellApp *app) {
    size_t length, count = 0, i;
    char *text = read_file(c, path, &length);
    IniReader *ini;
    Argument *args = NULL;
    unsigned seen_kind = 0, seen_pin = 0, seen_cols = 0, seen_rows = 0;
    int result = -1, n;
    if (!text) return -1;
    ini = ini_reader_with_limits(text, length, CATALOG_INPUT_LIMIT, CATALOG_LINE_LIMIT, ULONG_MAX);
    free(text);
    if (!ini) return fail(c, "Out of memory");
    app->terminal_cols = 80; app->terminal_rows = 30;
    while ((n = ini_next(ini)) == 1) {
        const char *key = ini_key(ini), *value = ini_value(ini);
        int bad = 0;
        if (strcmp(ini_section(ini), "application")) goto invalid;
        if (!strcmp(key, "id")) bad = assign(&app->id, value);
        else if (!strcmp(key, "name")) bad = assign(&app->name, value);
        else if (!strcmp(key, "icon")) bad = assign(&app->icon, value);
        else if (!strcmp(key, "category")) bad = assign(&app->category, value);
        else if (!strcmp(key, "keywords")) bad = assign(&app->keywords, value);
        else if (!strcmp(key, "exec")) bad = assign(&app->executable, value);
        else if (!strcmp(key, "fallback")) bad = assign(&app->fallback, value);
        else if (!strcmp(key, "kind")) {
            if (seen_kind++ || (strcmp(value, "gui") && strcmp(value, "terminal"))) goto invalid;
            app->kind = !strcmp(value, "terminal") ? SHELL_APP_TERMINAL : SHELL_APP_GUI;
        } else if (!strcmp(key, "pinned")) {
            if (seen_pin++ || (app->pinned_default = boolean(value)) < 0) goto invalid;
        } else if (!strcmp(key, "columns")) {
            if (seen_cols++ || geometry_value(value, 240, &app->terminal_cols)) goto invalid;
        } else if (!strcmp(key, "rows")) {
            if (seen_rows++ || geometry_value(value, 100, &app->terminal_rows)) goto invalid;
        } else if (!strncmp(key, "arg", 3)) {
            const char *p = key + 3;
            unsigned long index = 0;
            Argument *next;
            if (*p < '1' || *p > '9') goto invalid;
            while (*p >= '0' && *p <= '9') {
                unsigned digit = *p++ - '0';
                if (index > (ULONG_MAX - digit) / 10) goto invalid;
                index = index * 10 + digit;
            }
            if (*p || !valid_text(value, CATALOG_LINE_LIMIT, 1) || count >= (size_t)-1 / sizeof(*args) - 1) goto invalid;
            next = realloc(args, (count + 1) * sizeof(*args));
            if (!next) goto memory;
            args = next; args[count].index = index; args[count].value = copy(value);
            if (!args[count].value) goto memory;
            count++;
        } else goto invalid;
        if (bad) goto invalid;
    }
    if (n < 0) goto invalid;
    if (!app->id || !app->name || !app->icon || !app->category || !app->executable || !seen_kind ||
        !valid_id(app->id) || !valid_text(app->name, 64, 0) || !valid_text(app->icon, 128, 1) ||
        !valid_text(app->category, 64, 0) || !valid_path(app->executable, 0)) goto invalid;
    if (!app->keywords && assign(&app->keywords, "")) goto memory;
    if (!app->fallback && assign(&app->fallback, "")) goto memory;
    if (!valid_text(app->keywords, 1024, 1) || !valid_path(app->fallback, 1)) goto invalid;
    if (app->kind == SHELL_APP_GUI && (seen_cols || seen_rows)) goto invalid;
    if (count) {
        qsort(args, count, sizeof(*args), compare_arg);
        app->arguments = calloc(count, sizeof(*app->arguments));
        if (!app->arguments) goto memory;
        for (i = 0; i < count; i++) {
            if (args[i].index != i + 1) goto invalid;
            app->arguments[i] = args[i].value; args[i].value = NULL; app->argument_count++;
        }
    }
    result = 0; goto done;
invalid:
    errno = EINVAL; fail(c, "Invalid, duplicate or missing application field"); goto done;
memory:
    errno = ENOMEM; fail(c, "Out of memory");
done:
    for (i = 0; i < count; i++) free(args[i].value);
    free(args); ini_free(ini);
    if (result) app_free(app);
    return result;
}
static int append_file(ShellCatalog *target, ShellCatalog *staged, const char *path) {
    ShellApp app, *next;
    memset(&app, 0, sizeof(app));
    if (parse_app(target, path, &app)) return -1;
    if (shell_catalog_find(target, app.id) || shell_catalog_find(staged, app.id)) {
        app_free(&app); errno = EINVAL; return fail(target, "Duplicate application ID");
    }
    if (staged->count >= (size_t)-1 / sizeof(*next) - 1) { app_free(&app); errno = ENOMEM; return fail(target, "Catalog is too large"); }
    next = realloc(staged->apps, (staged->count + 1) * sizeof(*next));
    if (!next) { app_free(&app); return fail(target, "Out of memory"); }
    staged->apps = next; staged->apps[staged->count++] = app;
    return 0;
}
int shell_catalog_load(ShellCatalog *c, const char *path) {
    struct stat st;
    ShellCatalog *staged;
    ShellApp *next;
    size_t i;
    int bad = 0;
    if (!c || !path) { errno = EINVAL; return fail(c, "Invalid catalog path"); }
    c->error[0] = 0;
    if (stat(path, &st)) return fail(c, "Cannot find application catalog");
    staged = shell_catalog_new();
    if (!staged) return fail(c, "Out of memory");
    if (S_ISDIR(st.st_mode)) {
        DIR *dir = opendir(path);
        struct dirent *entry;
        if (!dir) bad = fail(c, "Cannot open application catalog directory");
        else {
            while (!bad && (entry = readdir(dir)) != NULL) {
                size_t n = strlen(entry->d_name), base = strlen(path);
                char *full;
                if (n < 5 || strcmp(entry->d_name + n - 4, ".ini")) continue;
                if (base > (size_t)-1 - n - 2) { bad = fail(c, "Catalog path is too long"); break; }
                full = malloc(base + n + 2);
                if (!full) { bad = fail(c, "Out of memory"); break; }
                memcpy(full, path, base); full[base] = '/'; memcpy(full + base + 1, entry->d_name, n + 1);
                bad = append_file(c, staged, full); free(full);
            }
            closedir(dir);
        }
    } else if (S_ISREG(st.st_mode)) bad = append_file(c, staged, path);
    else bad = fail(c, "Catalog is not a regular file or directory");
    if (!bad && staged->count) {
        if (c->count > (size_t)-1 / sizeof(*next) - staged->count) bad = fail(c, "Catalog is too large");
        else {
            next = realloc(c->apps, (c->count + staged->count) * sizeof(*next));
            if (!next) bad = fail(c, "Out of memory");
            else {
                c->apps = next;
                for (i = 0; i < staged->count; i++) c->apps[c->count++] = staged->apps[i];
                staged->count = 0;
                qsort(c->apps, c->count, sizeof(*c->apps), compare_app);
            }
        }
    }
    shell_catalog_free(staged); return bad;
}
int shell_pin_set(ShellCatalog *c, const char *id, int pinned) {
    size_t i;
    ShellPin *next;
    char *id_copy;
    if (!c || !valid_id(id) || (pinned != 0 && pinned != 1)) { errno = EINVAL; return fail(c, "Invalid pinned application"); }
    for (i = 0; i < c->pin_count; i++) if (!strcmp(c->pins[i].id, id)) { c->pins[i].value = pinned; return 0; }
    if (c->pin_count >= (size_t)-1 / sizeof(*next) - 1) return fail(c, "Too many pin preferences");
    id_copy = copy(id);
    if (!id_copy) return fail(c, "Out of memory");
    next = realloc(c->pins, (c->pin_count + 1) * sizeof(*next));
    if (!next) { free(id_copy); return fail(c, "Out of memory"); }
    c->pins = next; c->pins[c->pin_count].id = id_copy; c->pins[c->pin_count++].value = pinned;
    return 0;
}
int shell_app_pinned(const ShellCatalog *c, const ShellApp *app) {
    size_t i;
    if (!c || !app) return 0;
    for (i = 0; i < c->pin_count; i++) if (!strcmp(c->pins[i].id, app->id)) return c->pins[i].value;
    return app->pinned_default;
}
int shell_pins_load(ShellCatalog *c, const char *path) {
    size_t length, i;
    char *text;
    IniReader *ini;
    ShellCatalog *staged;
    int n, bad = 0;
    if (!c || !path) { errno = EINVAL; return fail(c, "Invalid pin configuration path"); }
    staged = shell_catalog_new();
    if (!staged) return fail(c, "Out of memory");
    text = read_file(c, path, &length);
    if (!text && errno != ENOENT) { shell_catalog_free(staged); return -1; }
    if (text) {
        ini = ini_reader_with_limits(text, length, CATALOG_INPUT_LIMIT, CATALOG_LINE_LIMIT, ULONG_MAX);
        free(text);
        if (!ini) { shell_catalog_free(staged); return fail(c, "Out of memory"); }
        while ((n = ini_next(ini)) == 1) {
            int value = boolean(ini_value(ini));
            const char *id = ini_key(ini);
            if (strcmp(ini_section(ini), "pins") || value < 0 || !valid_id(id)) { bad = 1; break; }
            for (i = 0; i < staged->pin_count; i++) if (!strcmp(staged->pins[i].id, id)) { bad = 1; break; }
            if (bad || shell_pin_set(staged, id, value)) { bad = 1; break; }
        }
        if (n < 0) bad = 1;
        ini_free(ini);
    }
    if (!bad) {
        for (i = 0; i < c->pin_count; i++) free(c->pins[i].id);
        free(c->pins); c->pins = staged->pins; c->pin_count = staged->pin_count;
        staged->pins = NULL; staged->pin_count = 0; c->error[0] = 0;
    } else { errno = EINVAL; fail(c, "Invalid or duplicate pin preference"); }
    shell_catalog_free(staged); return bad ? -1 : 0;
}
static int write_all(int fd, const char *text) {
    size_t left = strlen(text);
    while (left) {
        ssize_t written = write(fd, text, left);
        if (written < 0 && errno == EINTR) continue;
        if (written <= 0) return -1;
        left -= written; text += written;
    }
    return 0;
}
int shell_pins_save(ShellCatalog *c, const char *path) {
    size_t i, length;
    char *temporary;
    int fd, bad, saved;
    if (!c || !path || !(length = strlen(path)) || length > 4096) { errno = EINVAL; return fail(c, "Invalid pin configuration path"); }
    temporary = malloc(length + 32);
    if (!temporary) return fail(c, "Out of memory");
    snprintf(temporary, length + 32, "%s.tmp.%ld", path, (long)getpid());
    fd = open(temporary, O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (fd < 0) { free(temporary); return fail(c, "Cannot create pin configuration temporary file"); }
    bad = write_all(fd, "# Graphite dock preferences; overrides application defaults.\n[pins]\n");
    for (i = 0; !bad && i < c->pin_count; i++) {
        bad = write_all(fd, c->pins[i].id) || write_all(fd, c->pins[i].value ? "=true\n" : "=false\n");
    }
    /* The current C16 guest libc does not expose a linkable fsync. Writes are
     * unbuffered and rename still provides atomic replacement; crash durability
     * awaits a guest fsync implementation. Keep host durability where available. */
#if !defined(FUSE_X_USER)
    if (!bad && fsync(fd)) bad = 1;
#endif
    saved = errno;
    if (close(fd) && !bad) { bad = 1; saved = errno; }
    if (!bad && rename(temporary, path)) { bad = 1; saved = errno; }
    if (bad) unlink(temporary);
    free(temporary);
    if (bad) { errno = saved; return fail(c, "Cannot save pin configuration"); }
    c->error[0] = 0; return 0;
}
typedef struct RunArguments {
    char *text;
    char **values;
    size_t count;
} RunArguments;
static void run_arguments_free(RunArguments *args) {
    free(args->text); free(args->values);
    memset(args, 0, sizeof(*args));
}
static int run_arguments(ShellCatalog *c, const char *command, RunArguments *args) {
    const char *input;
    char *output;
    size_t length;
    memset(args, 0, sizeof(*args));
    if (!command || (length = strlen(command)) > CATALOG_LINE_LIMIT) {
        errno = EINVAL; return fail(c, "Run command exceeds 4096 cells");
    }
    args->text = malloc(length + 1);
    if (!args->text) return fail(c, "Out of memory");
    input = command; output = args->text;
    while (*input) {
        int quote = 0;
        char **next;
        while (*input == ' ' || *input == '\t') input++;
        if (!*input) break;
        if (args->count >= (size_t)-1 / sizeof(*next) - 1) goto memory;
        next = realloc(args->values, (args->count + 1) * sizeof(*next));
        if (!next) goto memory;
        args->values = next; args->values[args->count++] = output;
        while (*input && (quote || (*input != ' ' && *input != '\t'))) {
            int ch = (unsigned char)*input++;
            if (ch == quote) { quote = 0; continue; }
            if (!quote && (ch == '\'' || ch == '"')) { quote = ch; continue; }
            if (ch == '\\' && quote != '\'') {
                if (!*input) goto syntax;
                ch = (unsigned char)*input++;
            }
            if (ch < 0x20 || ch == 0x7f) goto syntax;
            *output++ = (char)ch;
        }
        if (quote) goto syntax;
        *output++ = 0;
        if (!valid_text(args->values[args->count - 1], CATALOG_LINE_LIMIT + 1, 1)) goto syntax;
    }
    if (!args->count || !args->values[0][0]) goto syntax;
    return 0;
syntax:
    run_arguments_free(args); errno = EINVAL;
    return fail(c, "Expected a program and balanced quotes; control characters are unsupported");
memory:
    run_arguments_free(args); errno = ENOMEM;
    return fail(c, "Out of memory");
}
static const char *basename_of(const char *path) {
    const char *slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}
/* Prefer an explicit ID or exact executable over a possibly ambiguous label. */
static const ShellApp *run_catalog_app(ShellCatalog *c, const char *name, int *ambiguous) {
    const ShellApp *match = shell_catalog_find(c, name);
    size_t i;
    int explicit_path = strchr(name, '/') != NULL;
    *ambiguous = 0;
    if (match) return match;
    for (i = 0; i < c->count; i++) {
        const ShellApp *app = c->apps + i;
        int matches = explicit_path ? !strcmp(name, app->executable) || (*app->fallback && !strcmp(name, app->fallback)) :
            !strcmp(name, basename_of(app->executable)) || (*app->fallback && !strcmp(name, basename_of(app->fallback)));
        if (matches) {
            /* Several descriptors may use the same interpreter with different
             * default args. An explicit program is still runnable; decline to
             * guess an identity and resolve it as an ordinary executable. */
            if (match) return NULL;
            match = app;
        }
    }
    if (match || explicit_path) return match;
    for (i = 0; i < c->count; i++) if (!compare_fold(name, c->apps[i].name)) {
        if (match) { *ambiguous = 1; return NULL; }
        match = c->apps + i;
    }
    return match;
}
static char *absolute_program(const char *program) {
    char cwd[1024], *path;
    size_t first, second;
    if (*program == '/') return copy(program);
    if (!getcwd(cwd, sizeof(cwd))) return NULL;
    first = strlen(cwd); second = strlen(program);
    if (first + second + 2 > sizeof(cwd)) { errno = ENAMETOOLONG; return NULL; }
    path = malloc(first + second + 2);
    if (!path) return NULL;
    memcpy(path, cwd, first); path[first] = '/'; memcpy(path + first + 1, program, second + 1);
    return path;
}
static char *run_program(ShellCatalog *c, const char *program) {
    const char *search;
    if (strchr(program, '/')) {
        char *absolute = absolute_program(program);
        if (!absolute) { fail(c, "Cannot resolve program path"); return NULL; }
        if (!valid_path(absolute, 0) || !available(absolute, "")) {
            free(absolute); errno = ENOENT; fail(c, "Program path is unavailable or not executable"); return NULL;
        }
        return absolute;
    }
    search = getenv("PATH");
    if (!search || !*search) search = "/usr/bin:/bin";
    for (;;) {
        const char *end = strchr(search, ':');
        size_t directory = end ? (size_t)(end - search) : strlen(search);
        size_t name = strlen(program);
        char *candidate, *absolute;
        if (directory + name + 2 < 1024) {
            candidate = malloc(directory + name + 3);
            if (!candidate) { fail(c, "Out of memory"); return NULL; }
            if (directory) { memcpy(candidate, search, directory); candidate[directory] = '/'; }
            memcpy(candidate + directory + (directory ? 1 : 0), program, name + 1);
            absolute = absolute_program(candidate); free(candidate);
            if (absolute) {
                if (valid_path(absolute, 0) && available(absolute, "")) return absolute;
                free(absolute);
            }
        }
        if (!end) break;
        search = end + 1;
    }
    errno = ENOENT; fail(c, "Program not found in PATH (or not executable)"); return NULL;
}
pid_t shell_catalog_run(ShellCatalog *c, const char *command, enum ShellRunMode mode) {
    RunArguments parsed;
    ShellApp app;
    const ShellApp *known;
    char *program = NULL, **arguments = NULL;
    char identity[64], name[64];
    size_t extra, base, i;
    unsigned long hash = 2166136261UL;
    int ambiguous;
    pid_t pid;
    if (!c || (mode != SHELL_RUN_AUTO && mode != SHELL_RUN_TERMINAL)) {
        errno = EINVAL; fail(c, "Invalid Run mode"); return -1;
    }
    if (run_arguments(c, command, &parsed)) return -1;
    known = run_catalog_app(c, parsed.values[0], &ambiguous);
    if (ambiguous) {
        run_arguments_free(&parsed); errno = EINVAL;
        fail(c, "Ambiguous application name; use its application ID or full executable path"); return -1;
    }
    if (!known || strchr(parsed.values[0], '/')) {
        program = run_program(c, parsed.values[0]);
        if (!program) { run_arguments_free(&parsed); return -1; }
    }
    if (known) app = *known;
    else {
        const char *label = basename_of(program);
        size_t length = strlen(label);
        memset(&app, 0, sizeof(app));
        for (i = 0; program[i]; i++) hash = ((hash ^ (unsigned char)program[i]) * 16777619UL) & 0xffffffffUL;
        snprintf(identity, sizeof(identity), "org.riplix.Run.%08lx", hash);
        if (length >= sizeof(name)) length = sizeof(name) - 1;
        memcpy(name, label, length); name[length] = 0;
        while (length && !valid_text(name, sizeof(name), 0)) name[--length] = 0;
        if (!length) strcpy(name, "Program");
        app.id = identity; app.name = name; app.icon = "terminal";
        app.category = "Run"; app.keywords = "";
        app.terminal_cols = 80; app.terminal_rows = 28;
        app.kind = SHELL_APP_GUI;
    }
    if (program) { app.executable = program; app.fallback = ""; }
    if (mode == SHELL_RUN_TERMINAL) app.kind = SHELL_APP_TERMINAL;
    base = app.argument_count; extra = parsed.count - 1;
    if (extra > (size_t)-1 / sizeof(*arguments) - base) {
        free(program); run_arguments_free(&parsed); errno = ENOMEM;
        fail(c, "Too many Run arguments"); return -1;
    }
    if (base + extra) {
        arguments = malloc((base + extra) * sizeof(*arguments));
        if (!arguments) { free(program); run_arguments_free(&parsed); fail(c, "Out of memory"); return -1; }
        for (i = 0; i < base; i++) arguments[i] = app.arguments[i];
        for (i = 0; i < extra; i++) arguments[base + i] = parsed.values[i + 1];
        app.arguments = arguments; app.argument_count = base + extra;
    }
    pid = shell_catalog_launch(c, &app);
    free(arguments); free(program); run_arguments_free(&parsed);
    return pid;
}
pid_t shell_catalog_launch(ShellCatalog *c, const ShellApp *app) {
    const char *program, *terminal;
    char **argv;
    char geometry[24];
    size_t n = 0, i, extra;
    int descriptors[2], flags, saved;
    pid_t pid;
    ShellChild *next, *child;
    if (!c || !app || !valid_id(app->id)) { errno = EINVAL; fail(c, "Invalid application"); return -1; }
    program = shell_app_available(c, app);
    terminal = available(c->terminal, c->terminal_fallback);
    if (!program) { errno = ENOENT; fail(c, "Application or terminal executable is unavailable"); return -1; }
    extra = app->kind == SHELL_APP_TERMINAL ? 12 : 2;
    if (app->argument_count > (size_t)-1 / sizeof(*argv) - extra || c->child_count >= (size_t)-1 / sizeof(*next) - 1) { errno = ENOMEM; fail(c, "Launch is too large"); return -1; }
    argv = calloc(app->argument_count + extra, sizeof(*argv));
    if (!argv) { fail(c, "Out of memory"); return -1; }
    if (app->kind == SHELL_APP_TERMINAL) {
        snprintf(geometry, sizeof(geometry), "%ux%u", app->terminal_cols, app->terminal_rows);
        argv[n++] = (char *)terminal; argv[n++] = "-g"; argv[n++] = geometry;
        argv[n++] = "--app-id"; argv[n++] = app->id;
        argv[n++] = "--app-name"; argv[n++] = app->name;
        argv[n++] = "--app-icon"; argv[n++] = app->icon; argv[n++] = "--";
    }
    argv[n++] = (char *)program;
    for (i = 0; i < app->argument_count; i++) argv[n++] = app->arguments[i];
    argv[n] = NULL;
    next = realloc(c->children, (c->child_count + 1) * sizeof(*next));
    if (!next) { free(argv); fail(c, "Out of memory"); return -1; }
    c->children = next;
    if (pipe(descriptors)) { free(argv); fail(c, "Cannot create launch status pipe"); return -1; }
    flags = fcntl(descriptors[1], F_GETFD, 0);
    if (flags < 0 || fcntl(descriptors[1], F_SETFD, flags | FD_CLOEXEC) < 0 ||
        (flags = fcntl(descriptors[0], F_GETFD, 0)) < 0 || fcntl(descriptors[0], F_SETFD, flags | FD_CLOEXEC) < 0 ||
        (flags = fcntl(descriptors[0], F_GETFL, 0)) < 0 || fcntl(descriptors[0], F_SETFL, flags | O_NONBLOCK) < 0) {
        saved = errno; close(descriptors[0]); close(descriptors[1]); free(argv); errno = saved; fail(c, "Cannot configure launch status pipe"); return -1;
    }
    pid = fork();
    if (pid < 0) { saved = errno; close(descriptors[0]); close(descriptors[1]); free(argv); errno = saved; fail(c, "Cannot create application process"); return -1; }
    if (!pid) {
        close(descriptors[0]);
        execv(argv[0], argv);
        saved = errno;
        /* Only a sizeof(int) record; interruption cannot lose the exec error. */
        {
            const char *p = (const char *)&saved;
            size_t remaining = sizeof(saved);
            while (remaining) {
                ssize_t written = write(descriptors[1], p, remaining);
                if (written < 0 && errno == EINTR) continue;
                if (written <= 0) break;
                p += written; remaining -= written;
            }
        }
        _exit(127);
    }
    close(descriptors[1]); free(argv);
    child = c->children + c->child_count++; memset(child, 0, sizeof(*child));
    child->pid = pid; child->fd = descriptors[0]; strcpy(child->app_id, app->id);
    c->error[0] = 0; return pid;
}
int shell_catalog_poll(ShellCatalog *c, ShellLaunchEvent *event) {
    size_t i;
    if (!c || !event) return 0;
    memset(event, 0, sizeof(*event));
    for (i = 0; i < c->child_count; i++) {
        ShellChild *child = c->children + i;
        int report = 0;
        if (child->fd >= 0) {
            ssize_t got = read(child->fd, (char *)&child->exec_error + child->received, sizeof(child->exec_error) - child->received);
            if (got > 0) child->received += got;
            if (!got || child->received == sizeof(child->exec_error) || (got < 0 && errno != EAGAIN && errno != EINTR)) {
                if (child->received && child->received != sizeof(child->exec_error)) child->exec_error = EIO;
                if (got < 0) child->exec_error = errno;
                close(child->fd); child->fd = -1;
            }
        }
        if (!child->reaped) {
            pid_t result = waitpid(child->pid, &child->status, WNOHANG);
            if (result == child->pid) child->reaped = 1;
            else if (result < 0 && errno == ECHILD) child->reaped = 1;
        }
        if (!child->announced && child->fd < 0) { child->announced = 1; report = child->exec_error ? SHELL_LAUNCH_FAILED : SHELL_LAUNCH_STARTED; }
        else if (child->announced && child->reaped) report = SHELL_LAUNCH_EXITED;
        if (report) {
            event->pid = child->pid; event->state = (enum ShellLaunchState)report;
            event->error = child->exec_error; event->status = child->status;
            strcpy(event->app_id, child->app_id);
            if (report == SHELL_LAUNCH_EXITED) {
                memmove(c->children + i, c->children + i + 1, (c->child_count - i - 1) * sizeof(*child)); c->child_count--;
            }
            return 1;
        }
    }
    return 0;
}
