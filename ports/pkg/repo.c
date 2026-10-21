#include "pkg.h"
#include <dirent.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
typedef struct {
    char name[31];
} Name;
static int compare(const void *a, const void *b) {
    return strcmp(((const Name *)a)->name, ((const Name *)b)->name);
}
int pkg_repo(const char *root) {
    DIR *dir = opendir(root);
    struct dirent *entry;
    Name *names = calloc(PKG_ENTRIES, sizeof(*names));
    Package *p = calloc(1, sizeof(*p));
    unsigned n = 0, i;
    char source[512], objects[512], dest[512], index[512], temporary[512];
    FILE *out = NULL;
    int failed = 1;
    if (!dir || !names || !p) {
        pkg_fail("cannot read repository", root);
        goto done;
    }
    while ((entry = readdir(dir)) != NULL) {
        size_t len = strlen(entry->d_name);
        struct stat st;
        if (len < 5 || strcmp(entry->d_name + len - 4, ".rpk"))
            continue;
        if (n == PKG_ENTRIES || len > 30) {
            pkg_fail("repository filename/count limit", entry->d_name);
            goto done;
        }
        if (pkg_join(source, root, entry->d_name) || lstat(source, &st) || !S_ISREG(st.st_mode)) {
            pkg_fail("repository package is not regular", entry->d_name);
            goto done;
        }
        strcpy(names[n++].name, entry->d_name);
    }
    closedir(dir);
    dir = NULL;
    qsort(names, n, sizeof(*names), compare);
    if (pkg_join(objects, root, "objects") || pkg_mkdirs(objects) ||
        pkg_join(index, root, "index.v1") || pkg_join(temporary, root, ".index.new"))
        goto done;
    out = fopen(temporary, "w");
    if (!out) {
        pkg_fail("cannot create repository index", temporary);
        goto done;
    }
    fputs("RIPINDEX1\n", out);
    for (i = 0; i < n; i++) {
        char id[29], hash[65];
        unsigned long size, existing;
        struct stat st;
        if (pkg_join(source, root, names[i].name) || pkg_inspect(source, p, NULL) ||
            pkg_hash_file(source, hash, &size))
            goto done;
        memcpy(id, hash, 24);
        strcpy(id + 24, ".rpk");
        if (pkg_join(dest, objects, id))
            goto done;
        if (lstat(dest, &st) == 0) {
            if (!S_ISREG(st.st_mode) || pkg_hash_file(dest, p->hash, &existing) ||
                strcmp(hash, p->hash) || size != existing) {
                pkg_fail("repository object collision", id);
                goto done;
            }
        } else if (pkg_copy(source, dest, 0644))
            goto done;
        if (fprintf(out, "%s\t%s\t%s\t%lu\t%s\tobjects/%s\t%s\n", p->name, p->version, p->target,
                    size, hash, id, p->depends) < 0)
            goto done;
    }
    if (fclose(out)) {
        out = NULL;
        goto done;
    }
    out = NULL;
    if (rename(temporary, index)) {
        pkg_fail("cannot publish repository index", index);
        goto done;
    }
    failed = 0;
done:
    if (dir)
        closedir(dir);
    if (out)
        fclose(out);
    free(names);
    free(p);
    return failed ? -1 : 0;
}
