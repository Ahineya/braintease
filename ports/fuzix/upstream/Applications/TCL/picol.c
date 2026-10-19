/* Tcl in ~ 500 lines of code.
 *
 * Copyright (c) 2007-2016, Salvatore Sanfilippo <antirez at gmail dot com>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *   * Redistributions of source code must retain the above copyright notice,
 *     this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <limits.h>
#include <ctype.h>
#define PICOL_LIMIT 30000
#define PICOL_WORDS 1024
#define PICOL_DEPTH 32

enum {PICOL_OK, PICOL_ERR, PICOL_RETURN, PICOL_BREAK, PICOL_CONTINUE};
enum {PT_ESC,PT_STR,PT_CMD,PT_VAR,PT_SEP,PT_EOL,PT_EOF};

struct picolParser {
    char *text;
    char *p; /* current text position */
    int len; /* remaining length */
    char *start; /* token start */
    char *end; /* token end */
    int type; /* token type, PT_... */
    const char *error;
    int haveword;
    int insidequote; /* True if inside " " */
};

struct picolVar {
    char *name, *val;
    struct picolVar *next;
};

struct picolInterp; /* forward declaration */
typedef int (*picolCmdFunc)(struct picolInterp *i, int argc, char **argv, void *privdata);

struct picolCmd {
    char *name;
    int active;
    picolCmdFunc func;
    void *privdata;
    struct picolCmd *next;
};

struct picolCallFrame {
    struct picolVar *vars;
    struct picolCallFrame *parent; /* parent is NULL at top level */
};

struct picolInterp {
    int exiting, exitcode;
    int level; /* Level of nesting */
    struct picolCallFrame *callframe;
    struct picolCmd *commands;
    char *result;
};

void picol_oom(void)
{
    write(2, "Out of memory\n", 14);
    exit(1);
}

void *picol_malloc(size_t s)
{
    void *p;
    if (s > PICOL_LIMIT + 1) picol_oom();
    p = malloc(s);
    if (p == NULL)
        picol_oom();
    return p;
}

char *picol_strdup(const char *p) {
    size_t n = strlen(p) + 1;
    char *r = picol_malloc(n);
    memcpy(r, p, n);
    return r;
}
void *picol_realloc(void *p, size_t n) {
    void *q;
    if (n > PICOL_LIMIT + 1) picol_oom();
    q = realloc(p, n);
    if (!q) picol_oom();
    return q;
}
static int expandLastWord(struct picolInterp *, int *, char ***);
static int picolCondition(struct picolInterp *, char *);
static int splitList(struct picolInterp *, const char *, int *, char ***);
static void freeWords(int, char **);
static char *makeList(int, char **);
static int picolInteger(struct picolInterp *, const char *, long *);
static int picolMath(struct picolInterp *, const char *, long, long, long *);

void picolInitParser(struct picolParser *p, char *text) {
    p->text = p->p = text;
    p->len = strlen(text);
    p->start = 0; p->end = 0; p->insidequote = 0;
    p->error = NULL; p->haveword = 0;
    p->type = PT_EOL;
}

int picolParseSep(struct picolParser *p) {
    p->start = p->p;
    while(*p->p == ' ' || *p->p == '\t' || *p->p == '\r') {
        p->p++; p->len--;
    }
    p->end = p->p-1;
    p->type = PT_SEP;
    return PICOL_OK;
}

int picolParseEol(struct picolParser *p) {
    p->start = p->p;
    while(*p->p == ' ' || *p->p == '\t' || *p->p == '\n' || *p->p == '\r' ||
          *p->p == ';')
    {
        p->p++; p->len--;
    }
    p->end = p->p-1;
    p->type = PT_EOL;
    return PICOL_OK;
}

int picolParseCommand(struct picolParser *p) {
    int level = 1;
    int blevel = 0;
    p->start = ++p->p; p->len--;
    while (1) {
        if (p->len == 0) {
            p->error = "missing close-bracket";
            return PICOL_ERR;
        } else if (*p->p == '[' && blevel == 0) {
            level++;
        } else if (*p->p == ']' && blevel == 0) {
            if (!--level) break;
        } else if (*p->p == '\\' && p->len >= 2) {
            p->p++; p->len--;
        } else if (*p->p == '{') {
            blevel++;
        } else if (*p->p == '}') {
            if (blevel != 0) blevel--;
        }
        p->p++; p->len--;
    }
    p->end = p->p-1;
    p->type = PT_CMD;
    if (*p->p == ']') {
        p->p++; p->len--;
    }
    return PICOL_OK;
}

int picolParseVar(struct picolParser *p) {
    p->start = ++p->p; p->len--;
    if (*p->p == '{') {
        p->start = ++p->p; p->len--;
        while (p->len && *p->p != '}') { p->p++; p->len--; }
        if (!p->len) { p->error = "missing variable close-brace"; return PICOL_ERR; }
        p->end = p->p - 1; p->p++; p->len--; p->type = PT_VAR;
        return PICOL_OK;
    }
    while (p->len && (isalnum(*p->p) || *p->p == '_')) { p->p++; p->len--; }
    if (p->len && *p->p == '(' && p->start != p->p) {
        p->p++; p->len--;
        while (p->len && *p->p != ')') { p->p++; p->len--; }
        if (!p->len) { p->error = "missing array close-parenthesis"; return PICOL_ERR; }
        p->p++; p->len--;
    }
    if (p->start == p->p) {
        p->start = p->end = p->p - 1; p->type = PT_STR;
    } else { p->end = p->p - 1; p->type = PT_VAR; }
    return PICOL_OK;
}

int picolParseBrace(struct picolParser *p) {
    int level = 1;
    p->start = ++p->p; p->len--;
    while(1) {
        if (p->len >= 2 && *p->p == '\\') {
            p->p++; p->len--;
        } else if (p->len == 0) {
            p->error = "missing close-brace";
            return PICOL_ERR;
        } else if (*p->p == '}') {
            level--;
            if (level == 0 || p->len == 0) {
                p->end = p->p-1;
                if (p->len) {
                    p->p++; p->len--; /* Skip final closed brace */
                }
                p->type = PT_STR;
                return PICOL_OK;
            }
        } else if (*p->p == '{')
            level++;
        p->p++; p->len--;
    }
    return PICOL_OK; /* unreached */
}

int picolParseString(struct picolParser *p) {
    int newword = (p->type == PT_SEP || p->type == PT_EOL || p->type == PT_STR);
    if (newword && *p->p == '{') return picolParseBrace(p);
    else if (newword && *p->p == '"') {
        p->insidequote = 1;
        p->p++; p->len--;
    }
    p->start = p->p;
    while(1) {
        if (p->len == 0) {
            p->end = p->p-1;
            p->type = PT_ESC;
            return PICOL_OK;
        }
        switch(*p->p) {
        case '\\':
            if (p->len >= 2) {
                p->p++; p->len--;
            }
            break;
        case '$': case '[':
            p->end = p->p-1;
            p->type = PT_ESC;
            return PICOL_OK;
        case ' ': case '\t': case '\n': case '\r': case ';':
            if (!p->insidequote) {
                p->end = p->p-1;
                p->type = PT_ESC;
                return PICOL_OK;
            }
            break;
        case '"':
            if (p->insidequote) {
                p->end = p->p-1;
                p->type = PT_ESC;
                p->p++; p->len--;
                p->insidequote = 0;
                return PICOL_OK;
            }
            break;
        }
        p->p++; p->len--;
    }
    return PICOL_OK; /* unreached */
}

int picolParseComment(struct picolParser *p) {
    while(p->len && *p->p != '\n') {
        p->p++; p->len--;
    }
    return PICOL_OK;
}

static int picolNextToken(struct picolParser *p) {
    while(1) {
        if (!p->len) {
            if (p->insidequote) {
                p->error = "missing close-quote";
                return PICOL_ERR;
            }
            if (p->type != PT_EOL && p->type != PT_EOF)
                p->type = PT_EOL;
            else
                p->type = PT_EOF;
            return PICOL_OK;
        }
        switch(*p->p) {
        case ' ': case '\t': case '\r':
            if (p->insidequote) return picolParseString(p);
            return picolParseSep(p);
        case '\n': case ';':
            if (p->insidequote) return picolParseString(p);
            return picolParseEol(p);
        case '[':
            return picolParseCommand(p);
        case '$':
            return picolParseVar(p);
        case '#':
            if (!p->haveword) {
                picolParseComment(p);
                continue;
            }
            return picolParseString(p);
        default:
            return picolParseString(p);
        }
    }
    return PICOL_OK; /* unreached */
}

int picolGetToken(struct picolParser *p) {
    int code = picolNextToken(p);
    if (p->type == PT_EOL) p->haveword = 0;
    else if (p->type != PT_SEP && p->type != PT_EOF) p->haveword = 1;
    return code;
}

void picolInitInterp(struct picolInterp *i) {
    i->level = i->exiting = i->exitcode = 0;
    i->callframe = picol_malloc(sizeof(struct picolCallFrame));
    i->callframe->vars = NULL;
    i->callframe->parent = NULL;
    i->commands = NULL;
    i->result = picol_strdup("");
}

void picolSetResult(struct picolInterp *i, char *s) {
    char *copy = picol_strdup(s);
    free(i->result);
    i->result = copy;
}

static char *environmentName(const char *name) {
    static char key[256]; size_t n = strlen(name);
    if (n < 6 || n > 260 || strncmp(name, "env(", 4) || name[n-1] != ')') return NULL;
    memcpy(key, name + 4, n - 5); key[n-5] = 0; return key;
}

struct picolVar *picolGetVar(struct picolInterp *i, char *name) {
    struct picolVar *v = i->callframe->vars;
    static struct picolVar environment;
    char *key = environmentName(name);
    if (key) { environment.val = getenv(key); return environment.val ? &environment : NULL; }
    while(v) {
        if (strcmp(v->name,name) == 0) return v;
        v = v->next;
    }
    return NULL;
}

int picolSetVar(struct picolInterp *i, char *name, char *val) {
    struct picolVar *v;
    char *key = environmentName(name);
    if (key) {
        static struct picolVar *exports;
        struct picolVar *owned = exports;
        char *pair = picol_malloc(strlen(key) + strlen(val) + 2);
        sprintf(pair, "%s=%s", key, val);
        if (putenv(pair)) { free(pair); picolSetResult(i, "cannot set environment variable"); return PICOL_ERR; }
        while (owned && strcmp(owned->name, key)) owned = owned->next;
        if (!owned) { owned = picol_malloc(sizeof(*owned)); owned->name = picol_strdup(key);
            owned->val = NULL; owned->next = exports; exports = owned; }
        free(owned->val); owned->val = pair;
        return PICOL_OK;
    }
    v = picolGetVar(i,name);
    if (v) {
        char *copy = picol_strdup(val);
        free(v->val);
        v->val = copy;
    } else {
        v = picol_malloc(sizeof(*v));
        v->name = picol_strdup(name);
        v->val = picol_strdup(val);
        v->next = i->callframe->vars;
        i->callframe->vars = v;
    }
    return PICOL_OK;
}

struct picolCmd *picolGetCommand(struct picolInterp *i, char *name) {
    struct picolCmd *c = i->commands;
    while(c) {
        if (strcmp(c->name,name) == 0) return c;
        c = c->next;
    }
    return NULL;
}

int picolRegisterCommand(struct picolInterp *i, char *name, picolCmdFunc f, void *privdata) {
    struct picolCmd *c = picolGetCommand(i,name);
    char errbuf[256];
    if (c) {
        snprintf(errbuf,sizeof(errbuf),"Command '%s' already defined",name);
        picolSetResult(i,errbuf);
        return PICOL_ERR;
    }
    c = picol_malloc(sizeof(*c));
    c->name = picol_strdup(name);
    c->active = 0;
    c->func = f;
    c->privdata = privdata;
    c->next = i->commands;
    i->commands = c;
    return PICOL_OK;
}

/* EVAL! Escape decoding happens only on substituting words, never braces. */
static void picolUnescape(char *s) {
    char *r = s, *w = s;
    while (*r) {
        char c = *r++;
        if (c == '\\' && *r) {
            c = *r++;
            if (c == 'n') c = '\n';
            else if (c == 'r') c = '\r';
            else if (c == 't') c = '\t';
            else if (c == '\n') {
                while (*r == ' ' || *r == '\t') r++;
                c = ' ';
            }
        }
        *w++ = c;
    }
    *w = 0;
}
int picolEval(struct picolInterp *i, char *script) {
    struct picolParser p;
    int argc = 0, j, retcode = PICOL_OK, expandword = 0;
    char **argv = NULL, errbuf[256];
    if (i->level >= PICOL_DEPTH) {
        picolSetResult(i, "evaluation nesting limit exceeded"); return PICOL_ERR;
    }
    if (strlen(script) > PICOL_LIMIT) {
        picolSetResult(i, "script exceeds 30000 characters"); return PICOL_ERR;
    }
    i->level++;
    picolSetResult(i, "");
    picolInitParser(&p, script);
    while (1) {
        char *token;
        int len, previous = p.type;
        if ((previous == PT_SEP || previous == PT_EOL) && p.len > 3 &&
            !strncmp(p.p, "{*}", 3) && !isspace(p.p[3]) && p.p[3] != ';') {
            p.p += 3; p.len -= 3; expandword = 1;
        }
        if (picolGetToken(&p) != PICOL_OK) {
            picolSetResult(i, (char *)p.error); retcode = PICOL_ERR; break;
        }
        if (expandword && (p.type == PT_SEP || p.type == PT_EOL || p.type == PT_EOF)) {
            retcode = expandLastWord(i, &argc, &argv); expandword = 0;
            if (retcode != PICOL_OK) break;
        }
        if (p.type == PT_EOF) break;
        if (p.type == PT_SEP) continue;
        if (p.type == PT_EOL) {
            struct picolCmd *c;
            if (argc) {
                c = picolGetCommand(i, argv[0]);
                if (!c) {
                    snprintf(errbuf, sizeof(errbuf), "No such command '%s'", argv[0]);
                    picolSetResult(i, errbuf); retcode = PICOL_ERR; break;
                }
                c->active++;
                retcode = c->func(i, argc, argv, c->privdata);
                c->active--;
                if (i->exiting) retcode = PICOL_RETURN;
                if (retcode != PICOL_OK) break;
            }
            for (j = 0; j < argc; j++) free(argv[j]);
            free(argv); argv = NULL; argc = 0;
            continue;
        }
        len = p.end - p.start + 1;
        if (len < 0) len = 0;
        token = picol_malloc(len + 1);
        memcpy(token, p.start, len); token[len] = 0;
        if (p.type == PT_VAR) {
            struct picolVar *v = picolGetVar(i, token);
            if (!v) {
                snprintf(errbuf, sizeof(errbuf), "No such variable '%s'", token);
                free(token); picolSetResult(i, errbuf); retcode = PICOL_ERR; break;
            }
            free(token); token = picol_strdup(v->val);
        } else if (p.type == PT_CMD) {
            retcode = picolEval(i, token); free(token);
            if (retcode != PICOL_OK) break;
            token = picol_strdup(i->result);
        } else if (p.type == PT_ESC) picolUnescape(token);
        if (previous == PT_SEP || previous == PT_EOL) {
            if (argc >= PICOL_WORDS) {
                free(token); picolSetResult(i, "too many arguments"); retcode = PICOL_ERR; break;
            }
            argv = picol_realloc(argv, sizeof(char *) * (argc + 1));
            argv[argc++] = token;
        } else {
            size_t old = strlen(argv[argc - 1]), add = strlen(token);
            argv[argc - 1] = picol_realloc(argv[argc - 1], old + add + 1);
            memcpy(argv[argc - 1] + old, token, add + 1); free(token);
        }
    }
    for (j = 0; j < argc; j++) free(argv[j]);
    free(argv); i->level--;
    return retcode;
}

/* ACTUAL COMMANDS! */
int picolArityErr(struct picolInterp *i, char *name) {
    char buf[256];
    snprintf(buf,sizeof(buf),"Wrong number of args for %s",name);
    picolSetResult(i,buf);
    return PICOL_ERR;
}

int picolCommandMath(struct picolInterp *i, int argc, char **argv, void *pd) {
    char buf[40]; long a, b, c;
    if (argc != 3) return picolArityErr(i, argv[0]);
    if (picolInteger(i, argv[1], &a) || picolInteger(i, argv[2], &b) ||
        picolMath(i, argv[0], a, b, &c)) return PICOL_ERR;
    snprintf(buf, sizeof(buf), "%ld", c); picolSetResult(i, buf);
    return PICOL_OK;
}

int picolCommandSet(struct picolInterp *i, int argc, char **argv, void *pd) {
    if (argc == 2) {
        struct picolVar *v = picolGetVar(i, argv[1]);
        if (!v) { picolSetResult(i, "No such variable"); return PICOL_ERR; }
        picolSetResult(i, v->val); return PICOL_OK;
    }
    if (argc != 3) return picolArityErr(i,argv[0]);
    picolSetVar(i,argv[1],argv[2]);
    picolSetResult(i,argv[2]);
    return PICOL_OK;
}

int picolCommandIf(struct picolInterp *i, int argc, char **argv, void *pd) {
    int truth;
    if (argc != 3 && argc != 5) return picolArityErr(i, argv[0]);
    if (argc == 5 && strcmp(argv[3], "else")) {
        picolSetResult(i, "expected else"); return PICOL_ERR;
    }
    truth = picolCondition(i, argv[1]);
    if (truth < 0) return PICOL_ERR;
    if (truth) return picolEval(i, argv[2]);
    if (argc == 5) return picolEval(i, argv[4]);
    picolSetResult(i, ""); return PICOL_OK;
}
int picolCommandWhile(struct picolInterp *i, int argc, char **argv, void *pd) {
    if (argc != 3) return picolArityErr(i, argv[0]);
    while (1) {
        int truth = picolCondition(i, argv[1]), code;
        if (truth < 0) return PICOL_ERR;
        if (!truth) { picolSetResult(i, ""); return PICOL_OK; }
        code = picolEval(i, argv[2]);
        if (code == PICOL_BREAK) { picolSetResult(i, ""); return PICOL_OK; }
        if (code != PICOL_OK && code != PICOL_CONTINUE) return code;
    }
}

int picolCommandRetCodes(struct picolInterp *i, int argc, char **argv, void *pd) {
    if (argc != 1) return picolArityErr(i,argv[0]);
    if (strcmp(argv[0],"break") == 0) return PICOL_BREAK;
    else if (strcmp(argv[0],"continue") == 0) return PICOL_CONTINUE;
    return PICOL_OK;
}

void picolDropCallFrame(struct picolInterp *i) {
    struct picolCallFrame *cf = i->callframe;
    struct picolVar *v = cf->vars, *t;
    while(v) {
        t = v->next;
        free(v->name);
        free(v->val);
        free(v);
        v = t;
    }
    i->callframe = cf->parent;
    free(cf);
}

int picolCommandCallProc(struct picolInterp *i, int argc, char **argv, void *pd) {
    char **data = pd, **formals;
    int n, arg, code;
    struct picolCallFrame *cf;
    if (splitList(i, data[0], &n, &formals)) return PICOL_ERR;
    if (n != argc - 1 && !(n && !strcmp(formals[n - 1], "args") && argc - 1 >= n - 1)) {
        freeWords(n, formals); return picolArityErr(i, argv[0]);
    }
    cf = picol_malloc(sizeof(*cf)); cf->vars = NULL;
    cf->parent = i->callframe; i->callframe = cf;
    for (arg = 0; arg < n; arg++) {
        if (arg == n - 1 && !strcmp(formals[arg], "args")) {
            char *tail = makeList(argc - 1 - arg, argv + 1 + arg);
            picolSetVar(i, formals[arg], tail); free(tail);
        } else picolSetVar(i, formals[arg], argv[arg + 1]);
    }
    freeWords(n, formals); code = picolEval(i, data[1]);
    if (code == PICOL_RETURN && !i->exiting) code = PICOL_OK;
    picolDropCallFrame(i); return code;
}

int picolCommandProc(struct picolInterp *i, int argc, char **argv, void *pd) {
    char **procdata;
    struct picolCmd *c;
    if (argc != 4) return picolArityErr(i, argv[0]);
    c = picolGetCommand(i, argv[1]);
    if (c && (c->func != picolCommandCallProc || c->active)) {
        picolSetResult(i, "cannot replace a builtin or active procedure"); return PICOL_ERR;
    }
    procdata = picol_malloc(sizeof(char *) * 2);
    procdata[0] = picol_strdup(argv[2]); procdata[1] = picol_strdup(argv[3]);
    if (c) {
        char **old = c->privdata;
        free(old[0]); free(old[1]); free(old); c->privdata = procdata;
        picolSetResult(i, ""); return PICOL_OK;
    }
    return picolRegisterCommand(i, argv[1], picolCommandCallProc, procdata);
}

int picolCommandReturn(struct picolInterp *i, int argc, char **argv, void *pd) {
    if (argc != 1 && argc != 2) return picolArityErr(i,argv[0]);
    picolSetResult(i, (argc == 2) ? argv[1] : "");
    return PICOL_RETURN;
}

void picolRegisterCoreCommands(struct picolInterp *i) {
    int j; char *name[] = {"+","-","*","/","%",">",">=","<","<=","==","!="};
    for (j = 0; j < (int)(sizeof(name)/sizeof(char*)); j++)
        picolRegisterCommand(i,name[j],picolCommandMath,NULL);
    picolRegisterCommand(i,"set",picolCommandSet,NULL);
    picolRegisterCommand(i,"if",picolCommandIf,NULL);
    picolRegisterCommand(i,"while",picolCommandWhile,NULL);
    picolRegisterCommand(i,"break",picolCommandRetCodes,NULL);
    picolRegisterCommand(i,"continue",picolCommandRetCodes,NULL);
    picolRegisterCommand(i,"proc",picolCommandProc,NULL);
    picolRegisterCommand(i,"return",picolCommandReturn,NULL);
}

/* Ripple CLI, expression, list and file-channel support. */
#include "picol-extra.inc"
