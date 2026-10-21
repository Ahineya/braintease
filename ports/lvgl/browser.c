/* SPDX-License-Identifier: MIT */
#include "browser.h"
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>

int browser_join(const char *directory, const char *name, char *out) {
    unsigned long n = strlen(name), d = strlen(directory);
    if (name[0] == '/') {
        if (n >= BROWSER_PATH) {
            errno = ENAMETOOLONG;
            return -1;
        }
        strcpy(out, name);
        return 0;
    }
    if (d + n + 1 + (d && directory[d - 1] != '/' ? 1 : 0) > BROWSER_PATH) {
        errno = ENAMETOOLONG;
        return -1;
    }
    strcpy(out, directory);
    if (d && out[d - 1] != '/')
        strcat(out, "/");
    strcat(out, name);
    return 0;
}
void browser_parent(const struct browser *b, char *out) {
    char *slash;
    strcpy(out, b->path);
    slash = strrchr(out, '/');
    if (slash == out)
        out[1] = 0;
    else if (slash)
        *slash = 0;
}
static int order(const void *aa, const void *bb) {
    const struct browser_entry *a = aa, *b = bb;
    int ad = a->kind == BROWSER_DIRECTORY, bd = b->kind == BROWSER_DIRECTORY;
    if (ad != bd)
        return bd - ad;
    return strcmp(a->name, b->name);
}
void browser_free(struct browser *b) {
    free(b->entries);
    b->entries = NULL;
    b->count = 0;
}
struct browser_job {
    char path[BROWSER_PATH];
    struct browser_entry *items, *scratch;
    DIR *dir;
    unsigned count, first, hidden, limited, phase, width, left, mid, end, i, j, k;
    int status, error, merging;
};
struct resolution {
    char todo[BROWSER_PATH], resolved[BROWSER_PATH], target[BROWSER_PATH], name[BROWSER_NAME];
};
/* Resolve components/symlinks without chdir, including the physical meaning
 * of link/.. . A lexical-only collapse can silently choose the wrong folder. */
static int canonical_path(const char *path, char *out) {
    struct resolution *r = calloc(1, sizeof(*r));
    unsigned links = 0;
    int error = 0;
    if (!r) {
        errno = ENOMEM;
        return -1;
    }
    if (!path || !*path)
        path = ".";
    if (path[0] == '/') {
        if (strlen(path) >= BROWSER_PATH) {
            error = ENAMETOOLONG;
            goto done;
        }
        strcpy(r->todo, path);
    } else {
        if (!getcwd(r->resolved, sizeof(r->resolved)) ||
            browser_join(r->resolved, path, r->todo) < 0) {
            error = errno;
            goto done;
        }
    }
    strcpy(r->resolved, "/");
    while (r->todo[0]) {
        struct stat st;
        unsigned at = 0, start, n, parent;
        while (r->todo[at] == '/')
            at++;
        start = at;
        while (r->todo[at] && r->todo[at] != '/')
            at++;
        n = at - start;
        if (!n)
            break;
        if (n >= BROWSER_NAME) {
            error = ENAMETOOLONG;
            goto done;
        }
        memcpy(r->name, r->todo + start, n);
        r->name[n] = 0;
        if (!strcmp(r->name, "..")) {
            char *slash = strrchr(r->resolved, '/');
            if (slash == r->resolved)
                r->resolved[1] = 0;
            else if (slash)
                *slash = 0;
        } else if (strcmp(r->name, ".")) {
            parent = strlen(r->resolved);
            if (parent + n + (parent > 1 ? 1 : 0) >= BROWSER_PATH) {
                error = ENAMETOOLONG;
                goto done;
            }
            if (parent > 1)
                strcat(r->resolved, "/");
            strcat(r->resolved, r->name);
            if (lstat(r->resolved, &st) < 0) {
                error = errno;
                goto done;
            }
            if (S_ISLNK(st.st_mode)) {
                int length;
                unsigned remaining;
                if (++links > 40) {
                    error = ELOOP;
                    goto done;
                }
                length = readlink(r->resolved, r->target, sizeof(r->target));
                if (length < 0) {
                    error = errno;
                    goto done;
                }
                if ((unsigned)length >= sizeof(r->target)) {
                    error = ENAMETOOLONG;
                    goto done;
                }
                r->target[length] = 0;
                remaining = strlen(r->todo + at);
                if ((unsigned)length + remaining >= BROWSER_PATH) {
                    error = ENAMETOOLONG;
                    goto done;
                }
                memmove(r->todo, r->todo + at, remaining + 1);
                memmove(r->todo + length, r->todo, remaining + 1);
                memcpy(r->todo, r->target, length);
                r->resolved[parent] = 0;
                if (r->target[0] == '/')
                    strcpy(r->resolved, "/");
                continue;
            }
            if (r->todo[at] && !S_ISDIR(st.st_mode)) {
                error = ENOTDIR;
                goto done;
            }
        }
        memmove(r->todo, r->todo + at, strlen(r->todo + at) + 1);
    }
    strcpy(out, r->resolved);
done:
    free(r);
    if (error) {
        errno = error;
        return -1;
    }
    return 0;
}
static int scan_fail(browser_job *job, int error) {
    job->error = error ? error : EIO;
    job->status = -1;
    if (job->dir) {
        closedir(job->dir);
        job->dir = NULL;
    }
    errno = job->error;
    return -1;
}
browser_job *browser_scan_begin(const char *path, int hidden) {
    browser_job *job = calloc(1, sizeof(*job));
    if (!job) {
        errno = ENOMEM;
        return NULL;
    }
    if (canonical_path(path, job->path) < 0)
        goto fail;
    job->dir = opendir(job->path);
    if (!job->dir)
        goto fail;
    job->items = calloc(BROWSER_LIMIT, sizeof(*job->items));
    job->scratch = calloc(BROWSER_LIMIT, sizeof(*job->scratch));
    if (!job->items || !job->scratch) {
        errno = ENOMEM;
        goto fail;
    }
    job->hidden = !!hidden;
    if (strcmp(job->path, "/")) {
        strcpy(job->items[0].name, "..");
        job->items[0].kind = BROWSER_DIRECTORY;
        job->count = job->first = 1;
    }
    return job;
fail:
    {
        int error = errno;
        browser_scan_delete(job);
        errno = error;
        return NULL;
    }
}
int browser_scan_step(browser_job *job, unsigned budget) {
    if (!job)
        return -1;
    if (job->status)
        return job->status;
    if (budget > 1024)
        budget = 1024;
    while (budget && !job->status) {
        if (!job->phase) {
            struct dirent *entry;
            struct stat st;
            char full[BROWSER_PATH];
            budget--;
            errno = 0;
            entry = readdir(job->dir);
            if (!entry) {
                int error = errno;
                if (closedir(job->dir) < 0 && !error)
                    error = errno;
                job->dir = NULL;
                if (error)
                    return scan_fail(job, error);
                job->phase = 1;
                job->width = 1;
                job->left = job->first;
                continue;
            }
            if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..") ||
                (!job->hidden && entry->d_name[0] == '.'))
                continue;
            if (job->count == BROWSER_LIMIT) {
                job->limited = 1;
                if (closedir(job->dir) < 0) {
                    job->dir = NULL;
                    return scan_fail(job, errno);
                }
                job->dir = NULL;
                job->phase = 1;
                job->width = 1;
                job->left = job->first;
                continue;
            }
            if (strlen(entry->d_name) >= BROWSER_NAME || browser_join(job->path, entry->d_name, full) < 0)
                return scan_fail(job, ENAMETOOLONG);
            strcpy(job->items[job->count].name, entry->d_name);
            if (!stat(full, &st)) {
                job->items[job->count].kind = S_ISDIR(st.st_mode) ? BROWSER_DIRECTORY
                                               : S_ISREG(st.st_mode) ? BROWSER_FILE : 0;
                job->items[job->count].size = st.st_size;
            }
            job->count++;
        } else {
            if (job->width >= job->count - job->first) {
                job->status = 1;
                break;
            }
            if (job->left >= job->count) {
                struct browser_entry *swap = job->items;
                job->items = job->scratch;
                job->scratch = swap;
                job->width *= 2;
                job->left = job->first;
                job->merging = 0;
                continue;
            }
            if (!job->merging) {
                if (job->first)
                    job->scratch[0] = job->items[0];
                job->mid = job->left + job->width;
                if (job->mid > job->count)
                    job->mid = job->count;
                job->end = job->left + job->width * 2;
                if (job->end > job->count)
                    job->end = job->count;
                job->i = job->k = job->left;
                job->j = job->mid;
                job->merging = 1;
            }
            if (job->k < job->end) {
                unsigned take = job->j == job->end ||
                                (job->i < job->mid && order(&job->items[job->i], &job->items[job->j]) <= 0)
                                    ? job->i++ : job->j++;
                job->scratch[job->k++] = job->items[take];
                budget--;
            } else {
                job->left = job->end;
                job->merging = 0;
            }
        }
    }
    return job->status;
}
int browser_scan_commit(browser_job *job, struct browser *b) {
    if (!job || !b || job->status != 1 || !job->items) {
        errno = EINVAL;
        return -1;
    }
    free(b->entries);
    b->entries = job->items;
    job->items = NULL;
    b->count = job->count;
    b->hidden = job->hidden;
    b->limited = job->limited;
    strcpy(b->path, job->path);
    return 0;
}
int browser_scan_error(const browser_job *job) { return job ? job->error : EINVAL; }
const char *browser_scan_path(const browser_job *job) { return job ? job->path : ""; }
void browser_scan_delete(browser_job *job) {
    if (!job)
        return;
    if (job->dir)
        closedir(job->dir);
    free(job->items);
    free(job->scratch);
    free(job);
}
static int scan_sync(struct browser *b, const char *path, int hidden, int change_cwd) {
    browser_job *job = browser_scan_begin(path, hidden);
    int result, error;
    if (!job)
        return -1;
    while (!(result = browser_scan_step(job, 128))) {}
    if (result > 0 && (!change_cwd || !chdir(job->path)))
        result = browser_scan_commit(job, b);
    else {
        error = result < 0 ? browser_scan_error(job) : errno;
        result = -1;
        errno = error;
    }
    error = errno;
    browser_scan_delete(job);
    errno = error;
    return result;
}
int browser_scan(struct browser *b, const char *path, int hidden) {
    return scan_sync(b, path, hidden, 0);
}
int browser_load(struct browser *b, const char *path, int hidden) {
    return scan_sync(b, path, hidden, 1);
}
int browser_utf8(uint32_t c, char out[5]) {
    unsigned n;
    if (!c || c > 0x10ffffUL || (c >= 0xd800UL && c <= 0xdfffUL))
        return 0;
    if (c < 128) {
        out[0] = c;
        n = 1;
    } else if (c < 2048) {
        out[0] = 0xc0 | (c >> 6);
        out[1] = 0x80 | (c & 63);
        n = 2;
    } else if (c < 65536UL) {
        out[0] = 0xe0 | (c >> 12);
        out[1] = 0x80 | ((c >> 6) & 63);
        out[2] = 0x80 | (c & 63);
        n = 3;
    } else {
        out[0] = 0xf0 | (c >> 18);
        out[1] = 0x80 | ((c >> 12) & 63);
        out[2] = 0x80 | ((c >> 6) & 63);
        out[3] = 0x80 | (c & 63);
        n = 4;
    }
    out[n] = 0;
    return n;
}
