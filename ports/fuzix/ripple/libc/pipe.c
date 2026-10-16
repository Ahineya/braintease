/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Process-local popen streams; retain child identity across unrelated waits. */
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
static struct { FILE *stream; pid_t child; } children[16];
FILE *popen(const char *command, const char *mode) {
    int fds[2], i, slot, parent_end, child_end, target, saved;
    pid_t child;
    FILE *stream;
    char *args[] = {"sh", "-c", (char *)command, NULL};
    if (!command || !mode || (strcmp(mode, "r") && strcmp(mode, "w"))) {
        errno = EINVAL;
        return NULL;
    }
    for (slot = 0; slot < 16 && children[slot].stream; slot++) {}
    if (slot == 16) { errno = EMFILE; return NULL; }
    if (fflush(NULL) || pipe(fds) < 0) return NULL;
    parent_end = mode[0] == 'r' ? 0 : 1;
    child_end = 1 - parent_end;
    target = mode[0] == 'r' ? 1 : 0;
    child = fork();
    if (child < 0) { saved = errno; close(fds[0]); close(fds[1]); errno = saved; return NULL; }
    if (!child) {
        close(fds[parent_end]);
        for (i = 0; i < 16; i++)
            if (children[i].stream) close(fileno(children[i].stream));
        if (fds[child_end] != target) {
            if (dup2(fds[child_end], target) < 0) _exit(126);
            close(fds[child_end]);
        }
        execv("/bin/sh", args);
        _exit(127);
    }
    close(fds[child_end]);
    stream = fdopen(fds[parent_end], mode);
    if (!stream) {
        saved = errno;
        close(fds[parent_end]);
        while (waitpid(child, NULL, 0) < 0 && errno == EINTR) {}
        errno = saved;
        return NULL;
    }
    children[slot].stream = stream;
    children[slot].child = child;
    return stream;
}
int pclose(FILE *stream) {
    int slot, status, saved, closed;
    pid_t child;
    for (slot = 0; slot < 16 && children[slot].stream != stream; slot++) {}
    if (!stream || slot == 16) { errno = EINVAL; return -1; }
    child = children[slot].child;
    children[slot].stream = NULL;
    closed = fclose(stream);
    saved = errno;
    while (waitpid(child, &status, 0) < 0)
        if (errno != EINTR) return -1;
    if (closed) { errno = saved; return -1; }
    return status;
}
