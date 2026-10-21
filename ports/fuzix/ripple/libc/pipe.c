/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Process-local popen streams; retain child identity across unrelated waits. */
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
struct pipe_child { FILE *stream; pid_t child; struct pipe_child *next; };
static struct pipe_child *children;
FILE *popen(const char *command, const char *mode) {
    int fds[2], parent_end, child_end, target, saved;
    struct pipe_child *entry, *prior;
    pid_t child;
    FILE *stream;
    char *args[] = {"sh", "-c", (char *)command, NULL};
    if (!command || !mode || (strcmp(mode, "r") && strcmp(mode, "w"))) {
        errno = EINVAL;
        return NULL;
    }
    if (fflush(NULL) || pipe(fds) < 0) return NULL;
    entry = malloc(sizeof(*entry));
    if (!entry) { close(fds[0]); close(fds[1]); errno = ENOMEM; return NULL; }
    parent_end = mode[0] == 'r' ? 0 : 1;
    child_end = 1 - parent_end;
    target = mode[0] == 'r' ? 1 : 0;
    child = fork();
    if (child < 0) { saved = errno; close(fds[0]); close(fds[1]); free(entry); errno = saved; return NULL; }
    if (!child) {
        close(fds[parent_end]);
        for (prior = children; prior; prior = prior->next)
            close(fileno(prior->stream));
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
        free(entry);
        errno = saved;
        return NULL;
    }
    entry->stream = stream;
    entry->child = child;
    entry->next = children;
    children = entry;
    return stream;
}
int pclose(FILE *stream) {
    int status, saved, closed;
    struct pipe_child **link = &children, *entry;
    pid_t child;
    while ((entry = *link) != NULL && entry->stream != stream) link = &entry->next;
    if (!stream || !entry) { errno = EINVAL; return -1; }
    child = entry->child;
    *link = entry->next;
    free(entry);
    closed = fclose(stream);
    saved = errno;
    while (waitpid(child, &status, 0) < 0)
        if (errno != EINTR) return -1;
    if (closed) { errno = saved; return -1; }
    return status;
}
