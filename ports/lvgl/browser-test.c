/* SPDX-License-Identifier: MIT */
#include "browser.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#define CHECK(x)                                                                                   \
    do {                                                                                           \
        if (!(x)) {                                                                                \
            fprintf(stderr, "BROWSER_MODEL failed line %d\n", __LINE__);                           \
            return 1;                                                                              \
        }                                                                                          \
    } while (0)
int main(int argc, char **argv) {
    struct browser b;
    char out[BROWSER_PATH], cwd[BROWSER_PATH], longname[BROWSER_PATH], utf8[5];
    struct browser_entry *old;
    unsigned i;
    memset(&b, 0, sizeof(b));
    CHECK(argc == 2 || argc == 3);
    CHECK(browser_load(&b, argv[1], 0) == 0);
    CHECK(b.count == 68 && !b.limited && !b.hidden);
    CHECK(!strcmp(b.entries[0].name, ".."));
    CHECK(!strcmp(b.entries[1].name, "Alpha"));
    CHECK(!strcmp(b.entries[2].name, "empty"));
    CHECK(!strcmp(b.entries[3].name, "zeta"));
    for (i = 4; i < b.count; i++)
        CHECK(b.entries[i].kind == BROWSER_FILE);
    CHECK(!strcmp(b.entries[4].name, "A file.txt"));
    browser_parent(&b, out);
    CHECK(strcmp(out, b.path));
    CHECK(browser_join(b.path, "Alpha", out) == 0);
    CHECK(browser_load(&b, out, 0) == 0 && b.count == 2);
    CHECK(!strcmp(b.entries[1].name, "nested.txt"));
    CHECK(browser_load(&b, "..", 1) == 0 && b.count == 69 && b.hidden);
    CHECK(!strcmp(b.entries[4].name, ".secret"));
    old = b.entries;
    CHECK(getcwd(cwd, sizeof(cwd)));
    CHECK(browser_load(&b, "does-not-exist", 0) < 0 && b.entries == old && b.count == 69);
    CHECK(getcwd(out, sizeof(out)) && !strcmp(out, cwd));
    CHECK(browser_load(&b, "last.txt", 0) < 0 && b.entries == old);
    CHECK(browser_load(&b, "empty", 0) == 0 && b.count == 1);
    CHECK(browser_load(&b, "..", 0) == 0 && b.count == 68);
    CHECK(browser_join(b.path, "/absolute/file", out) == 0 && !strcmp(out, "/absolute/file"));
    memset(longname, 'x', sizeof(longname) - 1);
    longname[sizeof(longname) - 1] = 0;
    CHECK(browser_join(b.path, longname, out) < 0 && errno == ENAMETOOLONG);
    longname[BROWSER_PATH - 2] = 0;
    CHECK(browser_join("/", longname, out) == 0 && strlen(out) == BROWSER_PATH - 1);
    CHECK(browser_utf8(0xe9, utf8) == 2 && (unsigned char)utf8[0] == 0xc3 &&
          (unsigned char)utf8[1] == 0xa9 && !utf8[2]);
    CHECK(browser_utf8(0x1f600UL, utf8) == 4 && (unsigned char)utf8[0] == 0xf0 &&
          (unsigned char)utf8[3] == 0x80);
    CHECK(!browser_utf8(0xd800UL, utf8) && !browser_utf8(0x110000UL, utf8) &&
          !browser_utf8(0, utf8));
    CHECK(browser_load(&b, "/", 0) == 0);
    browser_parent(&b, out);
    CHECK(!strcmp(out, "/"));
    for (i = 0; i < b.count; i++)
        CHECK(strcmp(b.entries[i].name, ".."));
    if (argc == 3) {
        CHECK(browser_load(&b, argv[2], 0) == 0 && b.count == BROWSER_LIMIT && b.limited);
    }
    browser_free(&b);
    CHECK(!b.entries && !b.count);
    puts("BROWSER_MODEL passed ordering, hidden files, traversal, failure preservation, path "
         "bounds and UTF-8");
    return 0;
}
