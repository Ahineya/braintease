/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Small foreground shell for the Ripple port; utilities are upstream programs.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>
static char line[512], words[2048];
static char *token[96];
static unsigned kind[96], nt, wp;
static int last_status, is_init;
static char *initial_env[] = {"PATH=/bin", "HOME=/", "USER=root", "TERM=ansi",
                              NULL};
enum { WORD, PIPE, INPUT, OUTPUT, APPEND, ERROR_OUTPUT };
struct command {
    char *argv[32];
    unsigned argc;
    char *in, *out, *err;
    int append;
};
static struct command commands[3];
static void failure(const char *s) { perror(s); }
static int putword(int c) {
    if (wp + 1 >= sizeof(words))
        return -1;
    words[wp++] = c;
    return 0;
}
/* Parse one semicolon-delimited pipeline. Expansion happens after its
 * predecessor. */
static char *lex(char *s) {
    nt = wp = 0;
    while (*s) {
        int quote = 0;
        while (*s == ' ' || *s == '\t')
            s++;
        if (!*s || *s == ';')
            break;
        if (nt == 95)
            goto bad;
        kind[nt] = WORD;
        token[nt] = words + wp;
        if (*s == '|') {
            kind[nt++] = PIPE;
            s++;
            continue;
        }
        if (*s == '<') {
            kind[nt++] = INPUT;
            s++;
            continue;
        }
        if (*s == '>') {
            s++;
            kind[nt++] = *s == '>' ? APPEND : OUTPUT;
            if (*s == '>')
                s++;
            continue;
        }
        if (s[0] == '2' && s[1] == '>') {
            kind[nt++] = ERROR_OUTPUT;
            s += 2;
            continue;
        }
        while (*s) {
            char c = *s++;
            if (!quote && (c == ' ' || c == '\t' || c == ';' || c == '|' ||
                           c == '<' || c == '>')) {
                s--;
                break;
            }
            if (c == '\'' && quote != '"') {
                quote = quote ? 0 : '\'';
                continue;
            }
            if (c == '"' && quote != '\'') {
                quote = quote ? 0 : '"';
                continue;
            }
            if (c == '\\' && quote != '\'') {
                if (!*s)
                    goto bad;
                c = *s++;
            } else if (c == '$' && quote != '\'') {
                char name[64], value[12], *p;
                unsigned n = 0;
                if (*s == '?') {
                    s++;
                    snprintf(value, sizeof(value), "%d", last_status);
                    p = value;
                } else {
                    while ((*s >= 'a' && *s <= 'z') ||
                           (*s >= 'A' && *s <= 'Z') ||
                           (*s >= '0' && *s <= '9') || *s == '_') {
                        if (n + 1 >= sizeof(name))
                            goto bad;
                        name[n++] = *s++;
                    }
                    if (!n) {
                        if (putword('$') < 0)
                            goto bad;
                        continue;
                    }
                    name[n] = 0;
                    p = getenv(name);
                }
                if (p)
                    while (*p)
                        if (putword(*p++) < 0)
                            goto bad;
                continue;
            }
            if (putword(c) < 0)
                goto bad;
        }
        if (quote || putword(0) < 0)
            goto bad;
        nt++;
    }
    return *s == ';' ? s + 1 : s;
bad:
    fputs("sh: malformed or overlong command\n", stderr);
    nt = 0;
    last_status = 2;
    return s + strlen(s);
}
static int parse(void) {
    unsigned i, n = 1;
    struct command *c;
    memset(commands, 0, sizeof(commands));
    c = commands;
    for (i = 0; i < nt; i++) {
        if (kind[i] == WORD) {
            if (c->argc == 31)
                return -1;
            c->argv[c->argc++] = token[i];
        } else if (kind[i] == PIPE) {
            if (!c->argc || n == 3)
                return -1;
            c = commands + n++;
        } else {
            unsigned op = kind[i];
            if (++i == nt || kind[i] != WORD)
                return -1;
            if (op == INPUT)
                c->in = token[i];
            else if (op == ERROR_OUTPUT)
                c->err = token[i];
            else {
                c->out = token[i];
                c->append = op == APPEND;
            }
        }
    }
    return c->argc ? n : -1;
}
static int redirect(const char *p, int fd, int flags) {
    int f = open(p, flags, 0666);
    if (f < 0) {
        failure(p);
        return -1;
    }
    if (f != fd) {
        if (dup2(f, fd) < 0) {
            close(f);
            failure("dup2");
            return -1;
        }
        close(f);
    }
    return 0;
}
static int builtin(struct command *c) {
    const char *s = c->argv[0];
    if (!strcmp(s, "cd")) {
        const char *p = c->argc > 1 ? c->argv[1] : "/";
        if (chdir(p) < 0) {
            failure(p);
            last_status = 1;
        } else
            last_status = 0;
    } else if (!strcmp(s, "shutdown") || !strcmp(s, "exit")) {
        if (!is_init && !strcmp(s, "exit"))
            exit(c->argc > 1 ? atoi(c->argv[1]) : last_status);
        if (uadmin(1, 0, NULL) < 0)
            failure("shutdown");
        last_status = 1;
    } else if (!strcmp(s, "help")) {
        puts("Commands: ls, cat, cp, mv, ln, rm, mkdir, "
             "rmdir, pwd,");
        puts("echo, head, tail, wc, grep, fgrep, sort, uniq, cut, tr, tee, "
             "cmp, chmod,");
        puts("touch, basename, dirname, seq, sleep, kill, sum, od, expand, "
             "colrm, which, vi, vile, ed.");
        puts("Shell: cd, sync, help, shutdown; quotes, <, >, >>, 2>, ;, $? "
             "and pipes.");
        puts("Editors: ed uses q to quit, Q to discard; vi uses Escape, then :wq or :q!.");
        last_status = 0;
    } else if (!strcmp(s, "sync")) {
        sync();
        puts("synced");
        last_status = 0;
    } else
        return 0;
    return 1;
}
static int is_builtin(const char *s) {
    return !strcmp(s, "cd") || !strcmp(s, "shutdown") || !strcmp(s, "exit") ||
           !strcmp(s, "help") || !strcmp(s, "sync");
}
static void execute(int n) {
    int fds[2][2], pids[3], i, j, status, started = 0;
    struct termios terminal;
    int restore_terminal;
    if (n == 1 && is_builtin(commands[0].argv[0])) {
        int saved[3] = {-1, -1, -1}, r = 0;
        struct command *c = commands;
        fflush(NULL);
        for (i = 0; i < 3; i++)
            if ((i == 0 && c->in) || (i == 1 && c->out) || (i == 2 && c->err)) {
                saved[i] = dup(i);
                if (saved[i] < 0) {
                    failure("dup");
                    r = -1;
                    break;
                }
            }
        if (!r && c->in)
            r = redirect(c->in, 0, O_RDONLY);
        if (!r && c->out)
            r = redirect(c->out, 1,
                         O_WRONLY | O_CREAT | (c->append ? O_APPEND : O_TRUNC));
        if (!r && c->err)
            r = redirect(c->err, 2, O_WRONLY | O_CREAT | O_TRUNC);
        if (!r)
            builtin(c);
        else
            last_status = 1;
        fflush(NULL);
        for (i = 0; i < 3; i++)
            if (saved[i] >= 0) {
                dup2(saved[i], i);
                close(saved[i]);
            }
        return;
    }
    for (i = 0; i < n; i++)
        if (is_builtin(commands[i].argv[0])) {
            fputs("sh: stateful builtins cannot be piped\n", stderr);
            last_status = 2;
            return;
        }
    restore_terminal = isatty(0) && tcgetattr(0, &terminal) == 0;
    for (i = 0; i < n - 1; i++)
        if (pipe(fds[i]) < 0) {
            failure("pipe");
            for (j = 0; j < i; j++) {
                close(fds[j][0]);
                close(fds[j][1]);
            }
            last_status = 1;
            return;
        }
    fflush(NULL);
    for (i = 0; i < n; i++) {
        int pid;
        pid = fork();
        if (pid < 0) {
            failure("fork");
            last_status = 1;
            break;
        }
        if (!pid) {
            struct command *c = commands + i;
            if ((i && dup2(fds[i - 1][0], 0) < 0) ||
                (i + 1 < n && dup2(fds[i][1], 1) < 0))
                _exit(1);
            for (j = 0; j < n - 1; j++) {
                close(fds[j][0]);
                close(fds[j][1]);
            }
            if (c->in && redirect(c->in, 0, O_RDONLY) < 0)
                _exit(1);
            if (c->out && redirect(c->out, 1,
                                   O_WRONLY | O_CREAT |
                                       (c->append ? O_APPEND : O_TRUNC)) < 0)
                _exit(1);
            if (c->err && redirect(c->err, 2, O_WRONLY | O_CREAT | O_TRUNC) < 0)
                _exit(1);
            execvp(c->argv[0], c->argv);
            failure(c->argv[0]);
            _exit(errno == ENOENT ? 127 : 126);
        }
        pids[started++] = pid;
    }
    for (j = 0; j < n - 1; j++) {
        close(fds[j][0]);
        close(fds[j][1]);
    }
    for (i = 0; i < started; i++) {
        int waited;
        do {
            waited = waitpid(pids[i], &status, 0);
        } while (waited < 0 && errno == EINTR);
        if (waited < 0) {
            failure("waitpid");
            last_status = 1;
        } else if (started == n && i == n - 1)
            last_status = WIFEXITED(status) ? WEXITSTATUS(status)
                                            : 128 + WTERMSIG(status);
    }
    /* PID 1 also adopts descendants of interrupted child shells/programs. */
    if (is_init)
        while (waitpid(-1, &status, WNOHANG) > 0)
            ;
    if (restore_terminal && tcsetattr(0, TCSANOW, &terminal) < 0)
        failure("terminal");
}
static void run_line(char *s) {
    int n;
    while (*s) {
        s = lex(s);
        if (!nt)
            continue;
        n = parse();
        if (n < 0) {
            fputs("sh: syntax error (at most three pipeline stages)\n", stderr);
            last_status = 2;
        } else
            execute(n);
    }
}
int main(int argc, char **argv, char **envp) {
    unsigned pos;
    int interactive;
    (void)envp;
    is_init = getpid() == 1;
    if (is_init) {
        environ = initial_env;
        if (open("/dev/tty1", O_RDWR) != 0)
            return 1;
        dup(0);
        dup(0);
    }
    if (argc > 1 && !strcmp(argv[1], "-c")) {
        if (argc != 3) {
            fputs("sh: usage: sh -c command\n", stderr);
            return 2;
        }
        run_line(argv[2]);
        return last_status;
    }
    if (argc > 1 && redirect(argv[1], 0, O_RDONLY) < 0)
        return 1;
    interactive = isatty(0);
    for (;;) {
        if (interactive) {
            fputs("fuzix# ", stdout);
            fflush(stdout);
        }
        pos = 0;
        /* No read-ahead: foreground children must be able to own stdin. */
        for (;;) {
            char c;
            long r = read(0, &c, 1);
            if (r < 0) {
                if (errno == EINTR) {
                    pos = 0;
                    last_status = 130;
                    putchar('\n');
                    break;
                }
                failure("read");
                return 1;
            }
            if (!r) {
                if (pos) {
                    line[pos] = 0;
                    run_line(line);
                }
                if (is_init)
                    uadmin(1, 0, NULL);
                return last_status;
            }
            if (c == '\n' || c == '\r')
                break;
            if (pos + 1 < sizeof(line))
                line[pos++] = c;
        }
        line[pos] = 0;
        run_line(line);
    }
}
