/* SPDX-License-Identifier: BSD-2-Clause
 * Build coordination commands for the guest Tcl subset.
 */
#include <dirent.h>
#include <sys/stat.h>

static int expandLastWord(struct picolInterp *i, int *argc, char ***argv) {
    char **words, **args = *argv; int n, j, old = *argc;
    if (!old) return failure(i, "missing expanded word");
    if (splitList(i, args[old - 1], &n, &words)) return PICOL_ERR;
    if (old - 1 + n > PICOL_WORDS) { freeWords(n, words); return failure(i, "too many expanded arguments"); }
    free(args[old - 1]);
    args = picol_realloc(args, (old + n) * sizeof(char *));
    for (j = 0; j < n; j++) args[old - 1 + j] = words[j];
    free(words); *argc = old - 1 + n; *argv = args; return PICOL_OK;
}

static char *pathJoin(const char *a, const char *b) {
    char *s; size_t n = strlen(a);
    if (*b == '/' || !n) return picol_strdup(b);
    s = picol_malloc(n + strlen(b) + 2);
    strcpy(s, a); if (s[n - 1] != '/') strcat(s, "/"); strcat(s, b); return s;
}

static int pathError(struct picolInterp *i, const char *path) {
    char text[512]; snprintf(text, sizeof(text), "%s: %s", path, strerror(errno));
    return failure(i, text);
}

static int makeDirectory(const char *name) {
    char *path = picol_strdup(name), *p; struct stat st; int code = 0;
    for (p = path + (*path == '/'); ; p++) {
        if (*p && *p != '/') continue;
        { char saved = *p; *p = 0;
          if (*path && mkdir(path, 0777) && (errno != EEXIST || stat(path, &st) || !S_ISDIR(st.st_mode))) code = -1;
          *p = saved; if (code || !saved) break; }
    }
    free(path); return code;
}

static int commandDirectory(struct picolInterp *i, int argc, char **argv, void *pd) {
    char text[1024];
    if (!strcmp(argv[0], "cd")) {
        const char *path = argc == 1 ? getenv("HOME") : argv[1];
        if (argc > 2 || !path) return picolArityErr(i, argv[0]);
        if (chdir(path)) return pathError(i, path);
        picolSetResult(i, ""); return PICOL_OK;
    }
    if (argc != 1) return picolArityErr(i, argv[0]);
    if (!getcwd(text, sizeof(text))) return pathError(i, "pwd");
    picolSetResult(i, text); return PICOL_OK;
}

static int commandFile(struct picolInterp *i, int argc, char **argv, void *pd) {
    struct stat st; const char *op; int j;
    if (argc < 3) return picolArityErr(i, argv[0]);
    op = argv[1];
    if (!strcmp(op, "join")) {
        char *s = picol_strdup(argv[2]);
        for (j = 3; j < argc; j++) { char *next = pathJoin(s, argv[j]); free(s); s = next; }
        picolSetResult(i, s); free(s); return PICOL_OK;
    }
    if (!strcmp(op, "dirname") || !strcmp(op, "tail")) {
        char *s, *slash; size_t n;
        if (argc != 3) return picolArityErr(i, argv[0]);
        s = picol_strdup(argv[2]); n = strlen(s);
        while (n > 1 && s[n - 1] == '/') s[--n] = 0;
        slash = strrchr(s, '/');
        if (!strcmp(op, "tail")) picolSetResult(i, slash ? slash + 1 : s);
        else { if (slash) { if (slash == s) slash++; *slash = 0; } picolSetResult(i, slash ? s : "."); }
        free(s); return PICOL_OK;
    }
    if (!strcmp(op, "mkdir")) {
        for (j = 2; j < argc; j++) if (makeDirectory(argv[j])) return pathError(i, argv[j]);
        picolSetResult(i, ""); return PICOL_OK;
    }
    if (!strcmp(op, "rename")) {
        if (argc != 4) return picolArityErr(i, argv[0]);
        if (stat(argv[3], &st) == 0) return failure(i, "rename destination already exists");
        if (rename(argv[2], argv[3])) return pathError(i, argv[2]);
        picolSetResult(i, ""); return PICOL_OK;
    }
    if (!strcmp(op, "delete")) {
        int force = !strcmp(argv[2], "-force"), first = force ? 3 : 2;
        for (j = first; j < argc; j++) {
            if (stat(argv[j], &st)) { if (force && errno == ENOENT) continue; return pathError(i, argv[j]); }
            if (S_ISDIR(st.st_mode) ? rmdir(argv[j]) : unlink(argv[j])) return pathError(i, argv[j]);
        }
        picolSetResult(i, ""); return PICOL_OK;
    }
    if (!strcmp(op, "exists") || !strcmp(op, "isfile") || !strcmp(op, "isdirectory")) {
        int ok;
        if (argc != 3) return picolArityErr(i, argv[0]);
        ok = stat(argv[2], &st) == 0;
        if (ok && !strcmp(op, "isfile")) ok = S_ISREG(st.st_mode);
        if (ok && !strcmp(op, "isdirectory")) ok = S_ISDIR(st.st_mode);
        numberResult(i, ok); return PICOL_OK;
    }
    if (argc != 3) return picolArityErr(i, argv[0]);
    if (stat(argv[2], &st)) return pathError(i, argv[2]);
    if (!strcmp(op, "mtime")) numberResult(i, st.st_mtime);
    else if (!strcmp(op, "size")) numberResult(i, st.st_size);
    else return failure(i, "unsupported file operation");
    return PICOL_OK;
}

/* Shell-style path matching. Leading dots require an explicit dot pattern. */
static int pathMatch(const char *p, const char *s) {
    while (*p) {
        if (*p == '*') { while (*p == '*') p++; if (!*p) return 1;
            do { if (pathMatch(p, s)) return 1; } while (*s++); return 0; }
        if (!*s) return 0;
        if (*p == '?') { p++; s++; continue; }
        if (*p == '[') {
            int found = 0, inverse; p++; inverse = *p == '!' || *p == '^'; if (inverse) p++;
            while (*p && *p != ']') {
                int a = *p++, b = a;
                if (*p == '-' && p[1] && p[1] != ']') { p++; b = *p++; }
                if (*s >= a && *s <= b) found = 1;
            }
            if (!*p || found == inverse) return 0;
            p++; s++; continue;
        }
        if (*p == '\\' && p[1]) p++;
        if (*p++ != *s++) return 0;
    }
    return !*s;
}

static int globPath(struct picolInterp *i, const char *base, const char *pattern,
                    int depth, int *count, char ***matches) {
    const char *rest = strchr(pattern, '/'); size_t n = rest ? (size_t)(rest - pattern) : strlen(pattern);
    char *part = picol_malloc(n + 1); int code = PICOL_OK;
    memcpy(part, pattern, n); part[n] = 0;
    if (depth > 16) { free(part); return failure(i, "glob nesting limit exceeded"); }
    if (strpbrk(part, "*?[")) {
        DIR *dir = opendir(*base ? base : "."); struct dirent *entry;
        if (!dir) { free(part); return errno == ENOENT || errno == ENOTDIR ? PICOL_OK : pathError(i, base); }
        while ((entry = readdir(dir))) {
            char *path;
            if (entry->d_name[0] == '.' && part[0] != '.') continue;
            if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..") || !pathMatch(part, entry->d_name)) continue;
            path = pathJoin(base, entry->d_name);
            code = globPath(i, path, rest ? rest + 1 : "", depth + 1, count, matches); free(path);
            if (code) break;
        }
        closedir(dir);
    } else if (*part) {
        char *path = pathJoin(base, part);
        code = globPath(i, path, rest ? rest + 1 : "", depth + 1, count, matches); free(path);
    } else {
        struct stat st;
        if (stat(base, &st) == 0) {
            if (*count == PICOL_WORDS) code = failure(i, "too many glob results");
            else { *matches = picol_realloc(*matches, (*count + 1) * sizeof(char *));
                (*matches)[(*count)++] = picol_strdup(base); }
        }
    }
    free(part); return code;
}

static int comparePaths(const void *a, const void *b) { return strcmp(*(char **)a, *(char **)b); }
static int commandGlob(struct picolInterp *i, int argc, char **argv, void *pd) {
    int first = 1, quiet = 0, n = 0, j, code; char **words = NULL, *text;
    if (first < argc && !strcmp(argv[first], "-nocomplain")) { quiet = 1; first++; }
    if (first < argc && !strcmp(argv[first], "--")) first++;
    if (first == argc) return picolArityErr(i, argv[0]);
    for (j = first; j < argc; j++) {
        code = globPath(i, *argv[j] == '/' ? "/" : "", argv[j] + (*argv[j] == '/'), 0, &n, &words);
        if (code) { freeWords(n, words); return code; }
    }
    if (!n && !quiet) { free(words); return failure(i, "no files matched glob pattern"); }
    qsort(words, n, sizeof(char *), comparePaths);
    text = makeList(n, words); freeWords(n, words); picolSetResult(i, text); free(text); return PICOL_OK;
}

/* A dictionary keeps the first key position and the last assigned value. */
static int uniqueDict(int n, char **words) {
    int read, write = 0, j;
    for (read = 0; read < n; read += 2) {
        for (j = 0; j < write; j += 2) if (!strcmp(words[j], words[read])) break;
        if (j < write) {
            free(words[read]); free(words[j + 1]); words[j + 1] = words[read + 1];
        } else {
            words[write++] = words[read]; words[write++] = words[read + 1];
        }
    }
    return write;
}

static int commandDict(struct picolInterp *i, int argc, char **argv, void *pd) {
    int n = 0, j, found = -1, code = PICOL_OK; char **words = NULL, *text; const char *op;
    struct picolVar *v;
    if (argc < 2) return picolArityErr(i, argv[0]);
    op = argv[1];
    if (!strcmp(op, "create")) {
        if (argc % 2) return failure(i, "dict create needs key/value pairs");
        n = argc - 2; words = picol_malloc((n + 1) * sizeof(char *));
        for (j = 0; j < n; j++) words[j] = picol_strdup(argv[j + 2]);
        n = uniqueDict(n, words);
        text = makeList(n, words); freeWords(n, words); picolSetResult(i, text); free(text); return PICOL_OK;
    }
    if (argc < 3) return picolArityErr(i, argv[0]);
    if (!strcmp(op, "set")) {
        if (argc != 5) return picolArityErr(i, argv[0]);
        v = picolGetVar(i, argv[2]); text = v ? v->val : "";
    } else text = argv[2];
    if (splitList(i, text, &n, &words)) return PICOL_ERR;
    if (n % 2) { freeWords(n, words); return failure(i, "invalid dictionary"); }
    n = uniqueDict(n, words);
    if (argc >= 4) for (j = 0; j < n; j += 2) if (!strcmp(words[j], argv[3])) found = j;
    if (!strcmp(op, "get") && argc == 4) {
        if (found < 0) code = failure(i, "dictionary key does not exist"); else picolSetResult(i, words[found + 1]);
    } else if (!strcmp(op, "exists") && argc == 4) numberResult(i, found >= 0);
    else if (!strcmp(op, "size") && argc == 3) numberResult(i, n / 2);
    else if (!strcmp(op, "keys") && argc == 3) {
        char **keys = picol_malloc((n / 2 + 1) * sizeof(char *));
        for (j = 0; j < n; j += 2) keys[j / 2] = words[j];
        text = makeList(n / 2, keys); free(keys); picolSetResult(i, text); free(text);
    } else if (!strcmp(op, "set") && argc == 5) {
        if (found < 0) {
            if (n + 2 > PICOL_WORDS) { freeWords(n, words); return failure(i, "dictionary too large"); }
            words = picol_realloc(words, (n + 2) * sizeof(char *)); found = n; n += 2;
            words[found] = picol_strdup(argv[3]); words[found + 1] = NULL;
        }
        free(words[found + 1]); words[found + 1] = picol_strdup(argv[4]);
        text = makeList(n, words); picolSetVar(i, argv[2], text); picolSetResult(i, text); free(text);
    } else code = failure(i, "unsupported dict operation or wrong argument count");
    freeWords(n, words); return code;
}

static void registerBuildCommands(struct picolInterp *i) {
    picolRegisterCommand(i, "file", commandFile, NULL);
    picolRegisterCommand(i, "glob", commandGlob, NULL);
    picolRegisterCommand(i, "pwd", commandDirectory, NULL);
    picolRegisterCommand(i, "cd", commandDirectory, NULL);
    picolRegisterCommand(i, "dict", commandDict, NULL);
}
