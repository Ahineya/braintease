/* SPDX-License-Identifier: GPL-2.0-only */
/* Bootstrap userspace for the Ripple port, using FUZIX system calls. */
#include "user.h"
static char line[256];
static char *args[24];
static char cwd[256] = "/";
static void failure(void) {
    out("error ");
    number(error);
    out("\n");
}
static void cat(const char *path) {
    int fd = open(path, 0, 0);
    char b[128];
    long n;
    if (fd < 0) {
        failure();
        return;
    }
    while ((n = read(fd, b, 128)) > 0)
        write(1, b, n);
    if (n < 0)
        failure();
    close(fd);
}
static void ls(const char *path) {
    struct dirent d;
    int fd = open(path, 0, 0);
    long n;
    if (fd < 0) {
        failure();
        return;
    }
    while ((n = call(24, fd, (long)&d, sizeof(d), 0)) > 0)
        if (d.ino) {
            unsigned len = 0;
            while (len < 30 && d.name[len])
                len++;
            write(1, d.name, len);
            out("\n");
        }
    close(fd);
}
static int tokenize(void) {
    unsigned n = 0;
    char *s = line;
    while (*s) {
        while (*s == ' ' || *s == '\t')
            s++;
        if (!*s)
            break;
        if (n == 23)
            break;
        args[n++] = s;
        while (*s && *s != ' ' && *s != '\t')
            s++;
        if (*s)
            *s++ = 0;
    }
    args[n] = NULL;
    return n;
}
static void echo(int n) {
    int i, fd = 1, end = n;
    for (i = 1; i < n; i++)
        if (!strcmp(args[i], ">") || !strcmp(args[i], ">>")) {
            if (i + 1 >= n) {
                out("missing filename\n");
                return;
            }
            fd = open(args[i + 1], 1 | 256 | (!strcmp(args[i], ">>") ? 4 : 1024), 0644);
            if (fd < 0) {
                failure();
                return;
            }
            end = i;
            break;
        }
    for (i = 1; i < end; i++) {
        if (i > 1)
            write(fd, " ", 1);
        write(fd, args[i], strlen(args[i]));
    }
    write(fd, "\n", 1);
    if (fd != 1)
        close(fd);
}
static void cp(const char *src, const char *dst) {
    char b[128];
    int a = open(src, 0, 0), d;
    long n;
    if (a < 0) {
        failure();
        return;
    }
    d = open(dst, 1 | 256 | 1024, 0644);
    if (d < 0) {
        failure();
        close(a);
        return;
    }
    while ((n = read(a, b, 128)) > 0)
        if (write(d, b, n) != n) {
            failure();
            break;
        }
    close(a);
    close(d);
}
static void run(int n) {
    long pid;
    char path[128];
    char *env[1];
    int status;
    unsigned i;
    env[0] = NULL;
    pid = call(32, 0, 0, 0, 0);
    if (pid < 0) {
        failure();
        return;
    }
    if (!pid) {
        if (args[0][0] == '/' || args[0][0] == '.')
            call(23, (long)args[0], (long)args, (long)env, 0);
        else {
            memcpy(path, "/bin/", 5);
            for (i = 0; args[0][i] && i < 120; i++)
                path[5 + i] = args[0][i];
            path[5 + i] = 0;
            call(23, (long)path, (long)args, (long)env, 0);
        }
        failure();
        call(0, 127, 0, 0, 0);
        for (;;)
            ;
    }
    if (call(55, pid, (long)&status, 0, 0) < 0)
        failure();
}
int main(int argc, char **argv, char **envp) {
    int n;
    unsigned pos;
    char c;
    long r;
    (void)argc;
    (void)argv;
    (void)envp;
    if (open("/dev/tty1", 2, 0) != 0)
        return 1;
    call(17, 0, 0, 0, 0);
    call(17, 0, 0, 0, 0);
    out("\nFUZIX/Ripple: writable persistent root\nType help for commands; shutdown saves the disk "
        "and stops the VM.\n");
    for (;;) {
        out("fuzix# ");
        pos = 0;
        while ((r = read(0, &c, 1)) > 0 && c != '\n' && c != '\r') {
            if (pos < 255)
                line[pos++] = c;
        }
        if (r < 0) {
            failure();
            continue;
        }
        if (!r) {
            call(57, 1, 0, 0, 0);
            for (;;)
                ;
        }
        line[pos] = 0;
        n = tokenize();
        if (!n)
            continue;
        if (!strcmp(args[0], "help"))
            out("ls [dir], cat file, echo text [> or >> file], cp src dst, mv src dst,\nmkdir dir, "
                "rm file, rmdir dir, cd dir, pwd, sync, ps, shutdown\nExternal programs: hello, "
                "forkcheck, preemptcheck, iotest\n");
        else if (!strcmp(args[0], "echo"))
            echo(n);
        else if (!strcmp(args[0], "ls"))
            ls(n > 1 ? args[1] : ".");
        else if (!strcmp(args[0], "cat") && n > 1)
            cat(args[1]);
        else if (!strcmp(args[0], "cp") && n > 2)
            cp(args[1], args[2]);
        else if (!strcmp(args[0], "mv") && n > 2) {
            if (call(3, (long)args[1], (long)args[2], 0, 0) < 0)
                failure();
        } else if (!strcmp(args[0], "rm") && n > 1) {
            if (call(6, (long)args[1], 0, 0, 0) < 0)
                failure();
        } else if (!strcmp(args[0], "mkdir") && n > 1) {
            if (call(51, (long)args[1], 0755, 0, 0) < 0)
                failure();
        } else if (!strcmp(args[0], "rmdir") && n > 1) {
            if (call(52, (long)args[1], 0, 0, 0) < 0)
                failure();
        } else if (!strcmp(args[0], "cd")) {
            const char *d = n > 1 ? args[1] : "/";
            if (call(10, (long)d, 0, 0, 0) < 0)
                failure();
            else {
                if (d[0] == '/') {
                    memcpy(cwd, d, strlen(d) + 1);
                } else if (!strcmp(d, "..")) {
                    unsigned l = (unsigned)strlen(cwd);
                    while (l > 1 && cwd[l - 1] != '/')
                        --l;
                    if (l > 1)
                        --l;
                    cwd[l] = 0;
                } else if (strcmp(d, ".")) {
                    unsigned l = (unsigned)strlen(cwd);
                    if (l > 1)
                        cwd[l++] = '/';
                    if (l + strlen(d) < 256)
                        memcpy(cwd + l, d, strlen(d) + 1);
                }
            }
        } else if (!strcmp(args[0], "pwd")) {
            out(cwd);
            out("\n");
        } else if (!strcmp(args[0], "sync")) {
            if (call(11, 0, 0, 0, 0) < 0)
                failure();
            else
                out("synced\n");
        } else if (!strcmp(args[0], "ps")) {
            out("shell pid ");
            number((unsigned)call(18, 0, 0, 0, 0));
            out("\n");
        } else if (!strcmp(args[0], "shutdown") || !strcmp(args[0], "exit")) {
            call(57, 1, 0, 0, 0);
            failure();
        } else
            run(n);
    }
}
