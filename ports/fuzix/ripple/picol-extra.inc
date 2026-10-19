#include <sys/wait.h>
/* SPDX-License-Identifier: BSD-2-Clause
 * Ripple additions to Salvatore Sanfilippo's Picol. See picol.c for its notice.
 */
static int failure(struct picolInterp *i, const char *message) {
    picolSetResult(i, (char *)message); return PICOL_ERR;
}
static void numberResult(struct picolInterp *i, long n) {
    char text[40]; snprintf(text, sizeof(text), "%ld", n); picolSetResult(i, text);
}
static int picolInteger(struct picolInterp *i, const char *s, long *n) {
    char *end;
    errno = 0; *n = strtol(s, &end, 0);
    if (end == s) return failure(i, "expected a 32-bit integer");
    while (isspace(*end)) end++;
    if (!*s || *end || errno == ERANGE)
        return failure(i, "expected a 32-bit integer");
    return PICOL_OK;
}
static int picolMath(struct picolInterp *i, const char *op, long a, long b, long *v) {
    if (!strcmp(op, "+")) {
        if ((b > 0 && a > LONG_MAX - b) || (b < 0 && a < LONG_MIN - b)) goto overflow;
        *v = a + b;
    } else if (!strcmp(op, "-")) {
        if ((b < 0 && a > LONG_MAX + b) || (b > 0 && a < LONG_MIN + b)) goto overflow;
        *v = a - b;
    } else if (!strcmp(op, "*")) {
        unsigned long x = a < 0 ? 0UL - (unsigned long)a : a;
        unsigned long y = b < 0 ? 0UL - (unsigned long)b : b;
        int negative = (a < 0) != (b < 0);
        unsigned long limit = (unsigned long)LONG_MAX + negative, product;
        if (y && x > limit / y) goto overflow;
        product = x * y;
        *v = negative ? product == 2147483648UL ? LONG_MIN : -(long)product : (long)product;
    } else if (!strcmp(op, "/") || !strcmp(op, "%")) {
        long r;
        if (!b) return failure(i, "division by zero");
        if (a == LONG_MIN && b == -1) goto overflow;
        r = a % b;
        *v = a / b;
        if (r && ((r < 0) != (b < 0))) { r += b; (*v)--; }
        if (*op == '%') *v = r;
    } else if (!strcmp(op, "==")) *v = a == b;
    else if (!strcmp(op, "!=")) *v = a != b;
    else if (!strcmp(op, "<=")) *v = a <= b;
    else if (!strcmp(op, ">=")) *v = a >= b;
    else if (!strcmp(op, "<")) *v = a < b;
    else if (!strcmp(op, ">")) *v = a > b;
    else if (!strcmp(op, "&&")) *v = a && b;
    else if (!strcmp(op, "||")) *v = a || b;
    else return failure(i, "unsupported operator");
    return PICOL_OK;
overflow:
    return failure(i, "integer overflow");
}
struct expression { struct picolInterp *i; const char *p; int depth, error; };
static void exprSpace(struct expression *e) { while (isspace(*e->p)) e->p++; }
static long exprBinary(struct expression *, int, int);
static long exprPrimary(struct expression *e, int active) {
    long value = 0; const char *start;
    exprSpace(e); start = e->p;
    if (++e->depth > PICOL_DEPTH) { e->error = 1; e->depth--; return 0; }
    if (*e->p == '(') {
        e->p++; value = exprBinary(e, 1, active); exprSpace(e);
        if (*e->p != ')') e->error = 1; else e->p++;
    } else if (*e->p == '!' || ((*e->p == '-' || *e->p == '+') && !isdigit(e->p[1]))) {
        char op = *e->p++;
        value = exprPrimary(e, active);
        if (!active) value = 0;
        else if (op == '-' && value == LONG_MIN) e->error = 1;
        else if (op == '-') value = -value;
        else if (op == '!') value = !value;
    } else if (*e->p == '[') {
        struct picolParser parser;
        char *text; size_t n;
        picolInitParser(&parser, (char *)e->p);
        if (picolParseCommand(&parser)) { e->error = 1; }
        else {
            n = parser.end - parser.start + 1;
            text = picol_malloc(n + 1); memcpy(text, parser.start, n); text[n] = 0;
            e->p = parser.p;
            if (active && (picolEval(e->i, text) || picolInteger(e->i, e->i->result, &value))) e->error = 1;
            free(text);
        }
    } else if (*e->p == '$') {
        char name[80]; unsigned n = 0; int brace;
        struct picolVar *var;
        e->p++; brace = *e->p == '{'; if (brace) e->p++;
        while (*e->p && (brace ? *e->p != '}' : isalnum(*e->p) || *e->p == '_')) {
            if (n < 79) name[n++] = *e->p; else e->error = 1;
            e->p++;
        }
        if (brace) { if (*e->p != '}') e->error = 1; else e->p++; }
        name[n] = 0;
        if (active) {
            var = picolGetVar(e->i, name);
            if (!n || !var || picolInteger(e->i, var->val, &value)) e->error = 1;
        }
    } else if (isdigit(*e->p) || ((*e->p == '+' || *e->p == '-') && isdigit(e->p[1]))) {
        char *end;
        errno = 0; value = strtol(e->p, &end, 0);
        if (end == e->p || (active && errno == ERANGE)) e->error = 1;
        e->p = end;
    } else if (!strncmp(e->p, "true", 4)) { value = 1; e->p += 4; }
    else if (!strncmp(e->p, "false", 5)) { value = 0; e->p += 5; }
    else e->error = 1;
    if (e->p == start) e->error = 1;
    e->depth--; return value;
}
static int exprOperator(const char *p, char *op) {
    int n = 1, precedence = 0;
    if (!strncmp(p, "||", 2)) precedence = 1, n = 2;
    else if (!strncmp(p, "&&", 2)) precedence = 2, n = 2;
    else if (!strncmp(p, "==", 2) || !strncmp(p, "!=", 2)) precedence = 3, n = 2;
    else if (*p == '<' || *p == '>') precedence = 4, n = p[1] == '=' ? 2 : 1;
    else if (*p == '+' || *p == '-') precedence = 5;
    else if (*p && strchr("*/%", *p)) precedence = 6;
    if (precedence) { memcpy(op, p, n); op[n] = 0; }
    return precedence;
}
static long exprBinary(struct expression *e, int minimum, int active) {
    long left = exprPrimary(e, active);
    while (!e->error) {
        char op[3]; int precedence, nextactive = active; long right;
        exprSpace(e); precedence = exprOperator(e->p, op);
        if (precedence < minimum) break;
        e->p += strlen(op);
        if ((!strcmp(op, "&&") && !left) || (!strcmp(op, "||") && left)) nextactive = 0;
        right = exprBinary(e, precedence + 1, nextactive);
        if (active && !e->error && picolMath(e->i, op, left, right, &left)) e->error = 1;
    }
    return left;
}
static int expressionValue(struct picolInterp *i, const char *s, long *v) {
    struct expression e;
    e.i = i; e.p = s; e.depth = e.error = 0;
    *v = exprBinary(&e, 1, 1); exprSpace(&e);
    if (e.error || *e.p) return failure(i, "invalid integer expression (or overflow/division by zero)");
    return PICOL_OK;
}
static int picolCondition(struct picolInterp *i, char *s) {
    long n; const char *p = s;
    while (isspace(*p)) p++;
    /* Retain the bundled Picol's prefix-command conditions as well. */
    if (*p && strchr("+-*/%<>=!", *p) && (isspace(p[1]) || (p[1] == '=' && isspace(p[2])))) {
        if (picolEval(i, s) || picolInteger(i, i->result, &n)) return -1;
    } else if (expressionValue(i, s, &n)) return -1;
    return n != 0;
}
static void freeWords(int n, char **v) { int j; for (j = 0; j < n; j++) free(v[j]); free(v); }
/* Tcl list words are decoded without command or variable substitution. */
static int splitList(struct picolInterp *i, const char *text, int *argc, char ***argv) {
    const char *p = text;
    int n = 0; char **words = NULL;
    while (*p) {
        char *word; unsigned length = 0; int brace = 0, braced = 0, quote = 0, closed = 0;
        while (isspace(*p)) p++;
        if (!*p) break;
        if (n == PICOL_WORDS) goto bad;
        word = picol_malloc(strlen(p) + 1);
        if (*p == '{') { brace = braced = 1; p++; }
        else if (*p == '"') { quote = 1; p++; }
        while (*p) {
            char c = *p++;
            if (c == '\\' && *p) { word[length++] = c; word[length++] = *p++; continue; }
            if (brace) {
                if (c == '{') brace++;
                if (c == '}' && !--brace) { closed = 1; break; }
            } else if (quote) { if (c == '"') { closed = 1; break; } }
            else if (isspace(c)) { closed = 1; break; }
            word[length++] = c;
        }
        word[length] = 0;
        if (((braced || quote) && !closed) || ((braced || quote) && *p && !isspace(*p))) {
            free(word); goto bad;
        }
        if (!braced) picolUnescape(word);
        {
            char *compact = picol_strdup(word);
            free(word); word = compact;
        }
        words = picol_realloc(words, (n + 1) * sizeof(char *)); words[n++] = word;
    }
    *argc = n; *argv = words; return PICOL_OK;
bad:
    freeWords(n, words); return failure(i, "invalid or oversized list");
}
static char *makeList(int argc, char **argv) {
    size_t size = 1; int j; char *result, *w;
    for (j = 0; j < argc; j++) {
        size += 2 * strlen(argv[j]) + 3;
        if (size > PICOL_LIMIT) picol_oom();
    }
    result = w = picol_malloc(size);
    for (j = 0; j < argc; j++) {
        const char *p = argv[j];
        if (j) *w++ = ' ';
        if (!*p) { *w++ = '{'; *w++ = '}'; }
        while (*p) {
            char c = *p++;
            if (c == '\n' || c == '\r' || c == '\t') {
                *w++ = '\\'; *w++ = c == '\n' ? 'n' : c == '\r' ? 'r' : 't';
            } else {
                if (strchr(" \\{}[]$;\"", c)) *w++ = '\\';
                *w++ = c;
            }
        }
    }
    *w = 0; return result;
}
static int commandExpr(struct picolInterp *i, int argc, char **argv, void *pd) {
    char *text, *p; size_t size = 1; int j; long v;
    if (argc < 2) return picolArityErr(i, argv[0]);
    for (j = 1; j < argc; j++) size += strlen(argv[j]) + 1;
    text = p = picol_malloc(size);
    for (j = 1; j < argc; j++) { if (j != 1) *p++ = ' '; strcpy(p, argv[j]); p += strlen(p); }
    j = expressionValue(i, text, &v); free(text);
    if (!j) numberResult(i, v);
    return j;
}
static int commandIncr(struct picolInterp *i, int argc, char **argv, void *pd) {
    struct picolVar *var; long value = 0, increment = 1; char text[40];
    if (argc != 2 && argc != 3) return picolArityErr(i, argv[0]);
    var = picolGetVar(i, argv[1]);
    if ((var && picolInteger(i, var->val, &value)) || (argc == 3 && picolInteger(i, argv[2], &increment)) ||
        picolMath(i, "+", value, increment, &value)) return PICOL_ERR;
    snprintf(text, sizeof(text), "%ld", value);
    picolSetVar(i, argv[1], text); picolSetResult(i, text); return PICOL_OK;
}
static int commandFor(struct picolInterp *i, int argc, char **argv, void *pd) {
    int code, truth;
    if (argc != 5) return picolArityErr(i, argv[0]);
    code = picolEval(i, argv[1]); if (code) return code;
    while (1) {
        truth = picolCondition(i, argv[2]);
        if (truth < 0) return PICOL_ERR;
        if (!truth) break;
        code = picolEval(i, argv[4]);
        if (code == PICOL_BREAK) break;
        if (code && code != PICOL_CONTINUE) return code;
        code = picolEval(i, argv[3]);
        if (code == PICOL_BREAK) break;
        if (code) return code;
    }
    picolSetResult(i, ""); return PICOL_OK;
}
static int commandList(struct picolInterp *i, int argc, char **argv, void *pd) {
    char **words = NULL, *text; int n = 0, code = PICOL_OK, j; long index;
    if (!strcmp(argv[0], "list")) {
        text = makeList(argc - 1, argv + 1); picolSetResult(i, text); free(text); return PICOL_OK;
    }
    if (!strcmp(argv[0], "lappend")) {
        struct picolVar *v;
        if (argc < 2) return picolArityErr(i, argv[0]);
        v = picolGetVar(i, argv[1]);
        if (v && splitList(i, v->val, &n, &words)) return PICOL_ERR;
        if (n + argc - 2 > PICOL_WORDS) { freeWords(n, words); return failure(i, "too many list elements"); }
        words = picol_realloc(words, (n + argc - 2) * sizeof(char *) + 1);
        for (j = 2; j < argc; j++) words[n++] = picol_strdup(argv[j]);
        text = makeList(n, words); freeWords(n, words);
        picolSetVar(i, argv[1], text); picolSetResult(i, text); free(text); return PICOL_OK;
    }
    if (argc < 2 || splitList(i, argv[1], &n, &words)) return argc < 2 ? picolArityErr(i, argv[0]) : PICOL_ERR;
    if (!strcmp(argv[0], "llength") && argc == 2) numberResult(i, n);
    else if (!strcmp(argv[0], "lindex") && argc == 3) {
        if (!strcmp(argv[2], "end")) index = n - 1;
        else if (picolInteger(i, argv[2], &index)) code = PICOL_ERR;
        if (!code) picolSetResult(i, index >= 0 && index < n ? words[index] : "");
    } else code = picolArityErr(i, argv[0]);
    freeWords(n, words); return code;
}
static int commandForeach(struct picolInterp *i, int argc, char **argv, void *pd) {
    int n, j, code = PICOL_OK; char **words;
    if (argc != 4) return picolArityErr(i, argv[0]);
    if (splitList(i, argv[2], &n, &words)) return PICOL_ERR;
    for (j = 0; j < n; j++) {
        picolSetVar(i, argv[1], words[j]); code = picolEval(i, argv[3]);
        if (code == PICOL_BREAK) { code = PICOL_OK; break; }
        if (code == PICOL_CONTINUE) code = PICOL_OK;
        if (code) break;
    }
    freeWords(n, words); if (!code) picolSetResult(i, ""); return code;
}
static FILE *channels[8];
static FILE *channel(struct picolInterp *i, const char *name) {
    if (!strcmp(name, "stdin")) return stdin;
    if (!strcmp(name, "stdout")) return stdout;
    if (!strcmp(name, "stderr")) return stderr;
    if (!strncmp(name, "file", 4) && name[4] >= '0' && name[4] <= '7' && !name[5] && channels[name[4] - '0'])
        return channels[name[4] - '0'];
    failure(i, "invalid channel"); return NULL;
}
static int commandPuts(struct picolInterp *i, int argc, char **argv, void *pd) {
    int arg = 1, newline = 1; FILE *f = stdout;
    if (arg < argc && !strcmp(argv[arg], "-nonewline")) { newline = 0; arg++; }
    if (argc - arg == 2) { f = channel(i, argv[arg++]); if (!f) return PICOL_ERR; }
    if (argc - arg != 1) return picolArityErr(i, argv[0]);
    if (fputs(argv[arg], f) == EOF || (newline && fputc('\n', f) == EOF)) return failure(i, "write failed");
    picolSetResult(i, ""); return PICOL_OK;
}
static int commandOpen(struct picolInterp *i, int argc, char **argv, void *pd) {
    int slot; const char *mode = argc == 3 ? argv[2] : "r"; char name[8];
    if (argc != 2 && argc != 3) return picolArityErr(i, argv[0]);
    if (strcmp(mode, "r") && strcmp(mode, "w") && strcmp(mode, "a") && strcmp(mode, "r+") && strcmp(mode, "w+") && strcmp(mode, "a+"))
        return failure(i, "unsupported open mode");
    for (slot = 0; slot < 8 && channels[slot]; slot++);
    if (slot == 8) return failure(i, "too many open channels");
    channels[slot] = fopen(argv[1], mode);
    if (!channels[slot]) return failure(i, "cannot open file");
    snprintf(name, sizeof(name), "file%d", slot); picolSetResult(i, name); return PICOL_OK;
}
static int commandChannel(struct picolInterp *i, int argc, char **argv, void *pd) {
    FILE *f; int c, n = 0, status; long limit = PICOL_LIMIT; char *text;
    if (argc < 2 || argc > 3) return picolArityErr(i, argv[0]);
    f = channel(i, argv[1]); if (!f) return PICOL_ERR;
    if (!strcmp(argv[0], "close")) {
        if (argc != 2 || f == stdin || f == stdout || f == stderr) return failure(i, "cannot close this channel");
        channels[argv[1][4] - '0'] = NULL;
        if (fclose(f)) return failure(i, "close failed");
        picolSetResult(i, ""); return PICOL_OK;
    }
    if (!strcmp(argv[0], "flush")) {
        if (argc != 2) return picolArityErr(i, argv[0]);
        if (fflush(f)) return failure(i, "flush failed");
        picolSetResult(i, ""); return PICOL_OK;
    }
    if (!strcmp(argv[0], "eof")) {
        if (argc != 2) return picolArityErr(i, argv[0]);
        numberResult(i, feof(f) != 0); return PICOL_OK;
    }
    if (!strcmp(argv[0], "read") && argc == 3 && (picolInteger(i, argv[2], &limit) || limit < 0 || limit > PICOL_LIMIT))
        return failure(i, "read count must be 0..30000");
    text = picol_malloc(limit + 1);
    while (n < limit && (c = fgetc(f)) != EOF) {
        if (!c) { free(text); return failure(i, "NUL characters are not supported in Tcl strings"); }
        if (!strcmp(argv[0], "gets") && c == '\n') break;
        text[n++] = c;
    }
    if (ferror(f)) { free(text); return failure(i, "read failed"); }
    if (n == limit && (!strcmp(argv[0], "gets") || argc == 2)) {
        c = fgetc(f);
        if (ferror(f)) { free(text); return failure(i, "read failed"); }
        if (c != EOF) { ungetc(c, f); free(text); return failure(i, "input exceeds 30000 characters"); }
    }
    text[n] = 0;
    if (!strcmp(argv[0], "gets") && argc == 3) {
        picolSetVar(i, argv[2], text); numberResult(i, n == 0 && feof(f) ? -1 : n);
    } else picolSetResult(i, text);
    free(text); return PICOL_OK;
}
/* Capture one external program without feeding its arguments to a shell. */
static int commandExec(struct picolInterp *i, int argc, char **argv, void *pd) {
    int pipefd[2], child, status = 0, failed = 0, overflow = 0, fd;
    char **args, *output; size_t n = 0; long got;
    int stream = 0, count = argc;
    if (argc >= 4 && !strcmp(argv[argc-2], ">@stdout") && !strcmp(argv[argc-1], "2>@stderr")) {
        stream = 1; count -= 2;
    }
    if (count < 2) return picolArityErr(i, argv[0]);
    args = picol_malloc(count * sizeof(char *));
    for (fd = 1; fd < count; fd++) args[fd - 1] = argv[fd];
    args[count - 1] = NULL;
    output = stream ? NULL : picol_malloc(PICOL_LIMIT + 1);
    fflush(stdout); fflush(stderr);
    if (!stream && pipe(pipefd)) { free(args); free(output); return failure(i, "pipe failed"); }
    child = fork();
    if (child == 0) {
        if (!stream && dup2(pipefd[1], 1) < 0) _exit(127);
        for (fd = 3; fd < 16; fd++) close(fd);
        execvp(args[0], args); _exit(127);
    }
    free(args); if (!stream) close(pipefd[1]);
    if (child < 0) { if (!stream) close(pipefd[0]); free(output); return failure(i, "fork failed"); }
    while (!stream) {
        char discard[128];
        got = read(pipefd[0], n < PICOL_LIMIT ? output + n : discard,
                   n < PICOL_LIMIT ? PICOL_LIMIT - n : sizeof(discard));
        if (got < 0 && errno == EINTR) continue;
        if (got < 0) { failed = 1; break; }
        if (!got) break;
        if (n < PICOL_LIMIT) n += got; else overflow = 1;
    }
    if (!stream) close(pipefd[0]);
    while (waitpid(child, &status, 0) < 0) { if (errno != EINTR) { failed = 1; break; } }
    if (failed || overflow || !WIFEXITED(status) || WEXITSTATUS(status)) {
        char *code[3], pid[24], detail[24];
        if (failed || overflow) {
            picolSetVar(i, "errorCode", failed ? "POSIX EIO" : "NONE");
            free(output);
            return failure(i, failed ? "external program I/O or wait failed" :
                           "program output exceeds 30000 characters; use >@stdout 2>@stderr");
        }
        snprintf(pid, sizeof(pid), "%d", child);
        snprintf(detail, sizeof(detail), "%d", WIFEXITED(status) ? WEXITSTATUS(status) : WTERMSIG(status));
        code[0] = WIFEXITED(status) ? "CHILDSTATUS" : "CHILDKILLED"; code[1] = pid; code[2] = detail;
        { char *text = makeList(3, code); picolSetVar(i, "errorCode", text); free(text); }
        free(output);
        { char message[120]; snprintf(message, sizeof(message), "external program %s %s", WIFEXITED(status) ? "exited with status" : "terminated by signal", detail); return failure(i, message); }
    }
    if (stream) { picolSetResult(i, ""); return PICOL_OK; }
    if (n && output[n - 1] == '\n') n--;
    output[n] = 0;
    if (strlen(output) != n) { free(output); return failure(i, "NUL in program output"); }
    picolSetResult(i, output); free(output); return PICOL_OK;
}
static int commandEval(struct picolInterp *i, int argc, char **argv, void *pd) {
    if (argc != 2) return picolArityErr(i, argv[0]);
    return picolEval(i, argv[1]);
}
static int commandCatch(struct picolInterp *i, int argc, char **argv, void *pd) {
    int code;
    if (argc != 2 && argc != 3) return picolArityErr(i, argv[0]);
    code = picolEval(i, argv[1]);
    if (i->exiting) return code;
    if (argc == 3) picolSetVar(i, argv[2], i->result);
    numberResult(i, code); return PICOL_OK;
}
static int commandExit(struct picolInterp *i, int argc, char **argv, void *pd) {
    long status = 0;
    if (argc != 1 && argc != 2) return picolArityErr(i, argv[0]);
    if (argc == 2 && (picolInteger(i, argv[1], &status) || status < 0 || status > 255)) return failure(i, "exit status must be 0..255");
    i->exiting = 1; i->exitcode = status; return PICOL_RETURN;
}
static int commandError(struct picolInterp *i, int argc, char **argv, void *pd) {
    return argc == 2 ? failure(i, argv[1]) : picolArityErr(i, argv[0]);
}
static int commandString(struct picolInterp *i, int argc, char **argv, void *pd) {
    long index;
    if (argc == 3 && !strcmp(argv[1], "length")) numberResult(i, strlen(argv[2]));
    else if (argc == 4 && !strcmp(argv[1], "equal")) numberResult(i, !strcmp(argv[2], argv[3]));
    else if (argc == 4 && !strcmp(argv[1], "index")) {
        char text[2] = {0, 0}; size_t n = strlen(argv[2]);
        if (!strcmp(argv[3], "end")) index = (long)n - 1;
        else if (picolInteger(i, argv[3], &index)) return PICOL_ERR;
        if (index >= 0 && index < n) text[0] = argv[2][index];
        picolSetResult(i, text);
    } else return failure(i, "supported string operations: length, equal, index");
    return PICOL_OK;
}
static int commandAppend(struct picolInterp *i, int argc, char **argv, void *pd) {
    struct picolVar *v; size_t length = 1; char *s, *p; int j;
    if (argc < 2) return picolArityErr(i, argv[0]);
    v = picolGetVar(i, argv[1]); if (v) length += strlen(v->val);
    for (j = 2; j < argc; j++) length += strlen(argv[j]);
    s = p = picol_malloc(length);
    if (v) { strcpy(p, v->val); p += strlen(p); }
    for (j = 2; j < argc; j++) { strcpy(p, argv[j]); p += strlen(p); }
    *p = 0; picolSetVar(i, argv[1], s); picolSetResult(i, s); free(s); return PICOL_OK;
}
static int evalFile(struct picolInterp *i, const char *name) {
    FILE *f = fopen(name, "r"); char *text; size_t n; int code, extra;
    if (!f) return failure(i, "cannot open script");
    text = picol_malloc(PICOL_LIMIT + 1);
    n = fread(text, 1, PICOL_LIMIT, f); extra = fgetc(f);
    code = ferror(f); fclose(f);
    if (code || extra != EOF) { free(text); return failure(i, "script read failed or exceeds 30000 characters"); }
    text[n] = 0;
    if (strlen(text) != n) { free(text); return failure(i, "NUL in script"); }
    code = picolEval(i, text); free(text); return code;
}
static int commandSource(struct picolInterp *i, int argc, char **argv, void *pd) {
    if (argc != 2) return picolArityErr(i, argv[0]);
    return evalFile(i, argv[1]);
}
#include "picol-build.inc"

static void registerExtras(struct picolInterp *i) {
    int j;
    registerBuildCommands(i);
    const char *names[] = {"expr", "incr", "for", "list", "llength", "lindex", "lappend", "foreach", "puts", "open", "close", "read", "gets", "eof", "flush", "source", "eval", "catch", "exit", "error", "string", "append", "exec"};
    picolCmdFunc funcs[] = {commandExpr, commandIncr, commandFor, commandList, commandList, commandList, commandList, commandForeach, commandPuts, commandOpen, commandChannel, commandChannel, commandChannel, commandChannel, commandChannel, commandSource, commandEval, commandCatch, commandExit, commandError, commandString, commandAppend, commandExec};
    for (j = 0; j < (int)(sizeof(funcs) / sizeof(funcs[0])); j++) picolRegisterCommand(i, (char *)names[j], funcs[j], NULL);
}
/* Use the parser to distinguish incomplete interactive input from hard errors. */
static int completeScript(char *text) {
    struct picolParser p;
    picolInitParser(&p, text);
    while (p.type != PT_EOF) if (picolGetToken(&p)) return 0;
    return 1;
}
int main(int argc, char **argv) {
    struct picolInterp interp;
    int code = PICOL_OK, interactive = argc == 1 && isatty(0), j;
    char *arguments, count[16];
    picolInitInterp(&interp); picolRegisterCoreCommands(&interp); registerExtras(&interp);
    picolSetVar(&interp, "argc", "0"); picolSetVar(&interp, "argv", ""); picolSetVar(&interp, "argv0", argv[0]);
    if (argc > 1 && !strcmp(argv[1], "--help")) {
        puts("Usage: tcl [-c script | file [arguments...]]\nPicol Tcl subset: integer expressions, variables, procedures, loops, lists and file I/O.\nNo arguments: read standard input; interactive quit: exit."); return 0;
    }
    if (argc > 1 && !strcmp(argv[1], "--version")) { puts("Picol (FUZIX/Ripple Tcl subset)"); return 0; }
    if (argc > 1 && !strcmp(argv[1], "-c")) {
        if (argc != 3) { fputs("tcl: -c needs one script\n", stderr); return 2; }
        code = picolEval(&interp, argv[2]);
    } else if (argc > 1) {
        arguments = makeList(argc - 2, argv + 2);
        snprintf(count, sizeof(count), "%d", argc - 2);
        picolSetVar(&interp, "argc", count); picolSetVar(&interp, "argv", arguments); free(arguments);
        picolSetVar(&interp, "argv0", argv[1]);
        code = evalFile(&interp, argv[1]);
    } else {
        char *input = picol_malloc(PICOL_LIMIT + 1); unsigned n = 0; int c;
        picolSetVar(&interp, "argc", "0"); picolSetVar(&interp, "argv", ""); picolSetVar(&interp, "argv0", argv[0]);
        while (!interp.exiting) {
            if (interactive) { fputs(n ? "> " : "tcl> ", stdout); fflush(stdout); }
            while ((c = fgetc(stdin)) != EOF && c != '\n') {
                if (n == PICOL_LIMIT) { code = failure(&interp, "input exceeds 30000 characters"); goto input_done; }
                if (!c) { code = failure(&interp, "NUL in input"); goto input_done; }
                input[n++] = c;
            }
            if (c == EOF && ferror(stdin)) { code = failure(&interp, "stdin read failed"); break; }
            if (c == EOF && !n) break;
            if (n == PICOL_LIMIT) { code = failure(&interp, "input exceeds 30000 characters"); break; }
            input[n++] = '\n'; input[n] = 0;
            if (!completeScript(input) && c != EOF) continue;
            code = picolEval(&interp, input); n = 0;
            if (code == PICOL_RETURN) break;
            if (code != PICOL_OK) {
                if (!interactive) break;
                fprintf(stderr, "tcl: %s\n", interp.result);
                code = PICOL_OK;
            } else if (interactive && !interp.exiting && *interp.result) puts(interp.result);
            if (c == EOF) break;
        }
input_done:
        free(input);
    }
    for (j = 0; j < 8; j++) if (channels[j] && fclose(channels[j])) code = failure(&interp, "close failed");
    if (fflush(stdout)) code = failure(&interp, "write failed");
    if (interp.exiting && code != PICOL_ERR) return interp.exitcode;
    if (code != PICOL_OK && code != PICOL_RETURN) {
        fprintf(stderr, "tcl: %s\n", *interp.result ? interp.result : "unexpected break or continue"); return 1;
    }
    return 0;
}
