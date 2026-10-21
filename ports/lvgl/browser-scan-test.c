/* SPDX-License-Identifier: MIT */
#include "browser.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define CHECK(x) do { if (!(x)) { \
    fprintf(stderr, "BROWSER_SCAN failed: %s line=%lu errno=%lu\n", #x, \
            (unsigned long)__LINE__, (unsigned long)errno); \
    exit(1); \
} } while (0)

static struct browser browser;
static char initial_cwd[BROWSER_PATH], expected_cwd[BROWSER_PATH], actual_cwd[BROWSER_PATH];
static char small[BROWSER_PATH], limit[BROWSER_PATH], path[BROWSER_PATH], target[BROWSER_PATH];
static unsigned checks, total_steps;

static void cwd_unchanged(void) {
    CHECK(getcwd(actual_cwd, sizeof(actual_cwd)));
    CHECK(!strcmp(actual_cwd, expected_cwd));
    checks++;
}

static unsigned long digest(const struct browser *b) {
    unsigned i, j;
    unsigned long hash = 0;
    for (i = 0; i < b->count; i++) {
        const struct browser_entry *entry = &b->entries[i];
        for (j = 0; entry->name[j]; j++)
            hash = hash * 33 + (unsigned char)entry->name[j];
        hash = hash * 33 + entry->size;
        hash = hash * 33 + entry->kind;
    }
    return hash;
}

static void preserved(const struct browser *before, unsigned long hash) {
    CHECK(browser.entries == before->entries);
    CHECK(browser.count == before->count && browser.hidden == before->hidden &&
          browser.limited == before->limited && !strcmp(browser.path, before->path));
    CHECK(digest(&browser) == hash);
    cwd_unchanged();
}

static int index_of(const char *name) {
    unsigned i;
    for (i = 0; i < browser.count; i++)
        if (!strcmp(browser.entries[i].name, name))
            return i;
    return -1;
}

static void name_diagnostic(const char *expected) {
    unsigned i, j;
    if (index_of(expected) >= 0)
        return;
    fprintf(stderr, "BROWSER_SCAN missing name cells:");
    for (j = 0; expected[j]; j++)
        fprintf(stderr, " %02lx", (unsigned long)(unsigned char)expected[j]);
    fprintf(stderr, "\n");
    for (i = 0; i < browser.count; i++) {
        fprintf(stderr, "BROWSER_SCAN entry %lu:", (unsigned long)i);
        for (j = 0; browser.entries[i].name[j]; j++)
            fprintf(stderr, " %02lx", (unsigned long)(unsigned char)browser.entries[i].name[j]);
        fprintf(stderr, "\n");
    }
}

static void sorted(void) {
    unsigned i;
    CHECK(browser.count && !strcmp(browser.entries[0].name, ".."));
    for (i = 2; i < browser.count; i++) {
        const struct browser_entry *a = &browser.entries[i - 1], *b = &browser.entries[i];
        CHECK(a->kind == BROWSER_DIRECTORY || b->kind != BROWSER_DIRECTORY);
        if ((a->kind == BROWSER_DIRECTORY) == (b->kind == BROWSER_DIRECTORY))
            CHECK(strcmp(a->name, b->name) < 0);
    }
}

static void scan(const char *input, int hidden, const char *canonical,
                 unsigned count, unsigned minimum_steps) {
    struct browser before = browser;
    unsigned long hash = digest(&browser);
    browser_job *job = browser_scan_begin(input, hidden);
    unsigned steps = 0;
    int status;
    CHECK(job);
    CHECK(!strcmp(browser_scan_path(job), canonical));
    CHECK(!browser_scan_error(job));
    preserved(&before, hash);
    CHECK(browser_scan_commit(job, &browser) == -1 && errno == EINVAL);
    preserved(&before, hash);
    CHECK(browser_scan_step(job, 0) == 0);
    preserved(&before, hash);
    do {
        status = browser_scan_step(job, 1);
        CHECK(status >= 0);
        CHECK(++steps < 50000);
        preserved(&before, hash);
    } while (!status);
    CHECK(steps >= minimum_steps);
    CHECK(browser_scan_step(job, 1) == 1 && !browser_scan_error(job));
    preserved(&before, hash);
    CHECK(browser_scan_commit(job, &browser) == 0);
    CHECK(browser.count == count && browser.hidden == !!hidden);
    CHECK(!strcmp(browser.path, canonical));
    cwd_unchanged();
    before = browser;
    hash = digest(&browser);
    CHECK(browser_scan_commit(job, &browser) == -1 && errno == EINVAL);
    preserved(&before, hash);
    browser_scan_delete(job);
    preserved(&before, hash);
    sorted();
    total_steps += steps;
}

static void fail(const char *input, int error) {
    struct browser before = browser;
    unsigned long hash = digest(&browser);
    browser_job *job;
    errno = 0;
    job = browser_scan_begin(input, 0);
    CHECK(!job && errno == error);
    preserved(&before, hash);
    errno = 0;
    CHECK(browser_scan(&browser, input, 0) == -1 && errno == error);
    preserved(&before, hash);
    errno = 0;
    CHECK(browser_load(&browser, input, 0) == -1 && errno == error);
    preserved(&before, hash);
}

static void cancel(const char *input, int finish) {
    struct browser before = browser;
    unsigned long hash = digest(&browser);
    browser_job *job = browser_scan_begin(input, 0);
    int status = 0;
    unsigned steps = 0;
    CHECK(job);
    preserved(&before, hash);
    do {
        status = browser_scan_step(job, 1);
        CHECK(status >= 0 && ++steps < 50000);
        preserved(&before, hash);
    } while (!status && (finish || steps < 3));
    CHECK(finish ? status == 1 : status == 0);
    browser_scan_delete(job);
    preserved(&before, hash);
}

int main(int argc, char **argv) {
    unsigned i;
    CHECK(argc == 3);
    CHECK(strlen(argv[1]) < sizeof(small) && strlen(argv[2]) < sizeof(limit));
    strcpy(small, argv[1]);
    strcpy(limit, argv[2]);
    CHECK(getcwd(initial_cwd, sizeof(initial_cwd)));
    strcpy(expected_cwd, initial_cwd);

    scan(small, 0, small, 14, 32);
    CHECK(!browser.limited && index_of(".secret") == -1);
    name_diagnostic("A file.txt");
    name_diagnostic("A Україна.txt");
    CHECK(index_of("A file.txt") >= 0 && index_of("A Україна.txt") >= 0);
    CHECK(index_of("café.txt") >= 0);
    for (i = 1; i <= 6; i++)
        CHECK(browser.entries[i].kind == BROWSER_DIRECTORY);
    CHECK(index_of("alias-rel") >= 0 && index_of("alias-abs") >= 0 && index_of("alias-up") >= 0);
    CHECK(index_of("loop-a") >= 0 && index_of("loop-b") >= 0 && index_of("dangling") >= 0);
    CHECK(browser.entries[index_of("alias-rel")].kind == BROWSER_DIRECTORY);
    CHECK(browser.entries[index_of("alias-abs")].kind == BROWSER_DIRECTORY);
    CHECK(browser.entries[index_of("alias-up")].kind == BROWSER_DIRECTORY);
    CHECK(browser.entries[index_of("loop-a")].kind == 0 && browser.entries[index_of("dangling")].kind == 0);
    cancel(limit, 0);
    cancel(small, 1);
    scan(small, 1, small, 15, 32);
    CHECK(index_of(".secret") >= 0);
    scan(small, 0, small, 14, 32);

    CHECK(browser_join(small, "alias-rel", path) == 0);
    CHECK(browser_join(small, "Alpha", target) == 0);
    scan(path, 0, target, 2, 1);
    CHECK(index_of("nested.txt") == 1);
    CHECK(browser_join(small, "alias-abs", path) == 0);
    CHECK(browser_join(small, "zeta", target) == 0);
    scan(path, 0, target, 1, 1);
    CHECK(browser_join(small, "alias-up/..", path) == 0);
    strcpy(target, small);
    *strrchr(target, '/') = 0;
    scan(path, 0, target, 3, 1);

    /* Relative traversal, physical link/.. resolution and null/dot inputs. */
    CHECK(chdir(small) == 0);
    strcpy(expected_cwd, small);
    CHECK(browser_join(small, "Alpha", target) == 0);
    scan("alias-rel", 0, target, 2, 1);
    CHECK(browser_join(small, "empty", target) == 0);
    scan("alias-rel/../empty/.", 0, target, 1, 1);
    scan(".", 0, small, 14, 32);
    scan(NULL, 0, small, 14, 32);
    fail("missing", ENOENT);
    fail("dangling", ENOENT);
    fail("loop-a", ELOOP);
    fail("last.txt", ENOTDIR);
    fail("last.txt/..", ENOTDIR);
    memset(path, 'x', sizeof(path) - 1);
    path[sizeof(path) - 1] = 0;
    fail(path, ENAMETOOLONG);

    scan(limit, 0, limit, BROWSER_LIMIT, BROWSER_LIMIT * 5);
    CHECK(browser.limited && !browser.hidden);
    for (i = 1; i < browser.count; i++)
        CHECK(browser.entries[i].kind == BROWSER_FILE);
    cancel(small, 0);
    scan(small, 0, small, 14, 32);

    /* Legacy load keeps its successful chdir contract, using physical paths. */
    CHECK(browser_load(&browser, "alias-rel", 0) == 0 && browser.count == 2);
    CHECK(browser_join(small, "Alpha", expected_cwd) == 0);
    CHECK(!strcmp(browser.path, expected_cwd));
    cwd_unchanged();
    fail("missing", ENOENT);
    CHECK(browser_load(&browser, "..", 1) == 0 && browser.count == 15 && browser.hidden);
    strcpy(expected_cwd, small);
    cwd_unchanged();
    CHECK(browser_load(&browser, "alias-abs", 0) == 0 && browser.count == 1);
    CHECK(browser_join(small, "zeta", expected_cwd) == 0);
    CHECK(!strcmp(browser.path, expected_cwd));
    cwd_unchanged();
    CHECK(browser_scan(&browser, small, 1) == 0 && browser.count == 15 && browser.hidden);
    cwd_unchanged();
    CHECK(!strcmp(browser.path, small));
    browser_free(&browser);
    CHECK(!browser.entries && !browser.count);
    browser_scan_delete(NULL);
    CHECK(browser_scan_error(NULL) == EINVAL && !strcmp(browser_scan_path(NULL), ""));
    CHECK(chdir(initial_cwd) == 0);
    strcpy(expected_cwd, initial_cwd);
    cwd_unchanged();
    printf("BROWSER_SCAN passed checks=%u budget_one_steps=%u\n", checks, total_steps);
    return 0;
}
