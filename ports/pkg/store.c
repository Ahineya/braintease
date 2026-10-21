#include "pkg.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

typedef struct {
    char root[256], base[512], db[512], objects[512], txn[512];
    int lock;
} Store;
static int exists(const char *path) {
    struct stat st;
    return lstat(path, &st) == 0;
}
static Store *open_store(const char *root) {
    Store *s = calloc(1, sizeof(*s));
    char path[512];
    struct stat st;
    if (!s) {
        pkg_fail("out of memory", "");
        return NULL;
    }
    s->lock = -1;
    if (strlen(root) >= sizeof(s->root) || lstat(root, &st) || !S_ISDIR(st.st_mode)) {
        pkg_fail("installation root must exist", root);
        goto failed;
    }
    strcpy(s->root, root);
    if (pkg_join(s->base, root, "var/lib/pkg") || pkg_mkdirs(s->base) ||
        pkg_join(s->db, s->base, "db") || pkg_mkdirs(s->db) ||
        pkg_join(s->objects, s->base, "objects") || pkg_mkdirs(s->objects) ||
        pkg_join(s->txn, s->base, "txn") || pkg_join(path, s->base, "lock"))
        goto failed;
    s->lock = open(path, O_RDWR | O_CREAT, 0600);
    if (s->lock < 0 || flock(s->lock, LOCK_EX | LOCK_NB)) {
        pkg_fail("package database is locked", path);
        goto failed;
    }
    return s;
failed:
    if (s->lock >= 0)
        close(s->lock);
    free(s);
    return NULL;
}
static void close_store(Store *s) {
    if (s) {
        if (s->lock >= 0)
            close(s->lock);
        free(s);
    }
}
static int dbpath(char out[512], Store *s, const char *name, const char *suffix) {
    char file[40];
    if (!pkg_token(name, 24) || !strcmp(name, ".") || !strcmp(name, ".."))
        return pkg_fail("invalid package name", name);
    strcpy(file, name);
    strcat(file, suffix);
    return pkg_join(out, s->db, file);
}
static int object_path(char out[512], Store *s, const Package *p, const char *suffix) {
    char id[32], base[512];
    memcpy(id, p->hash, 24);
    id[24] = 0;
    if (pkg_join(base, s->objects, id))
        return -1;
    return pkg_join(out, base, suffix);
}
static int permitted(const Package *p, const char *path) {
    static const char *prefixes[] = {"usr/bin/",   "usr/libexec/", "usr/lib/",
                                     "usr/man/",   "usr/src/",     "usr/include/",
                                     "usr/share/", "lib/fuse-x/",  "lib/riplix/"};
    unsigned i;
    if (!strncmp(path, "etc/", 4))
        return 1;
    if (!strncmp(path, "bin/", 4)) {
        char counterpart[256];
        if (!strcmp(path, "bin/sh") || !strcmp(path, "bin/svcd") ||
            !strcmp(path, "bin/console-session"))
            return 0;
        snprintf(counterpart, sizeof(counterpart), "usr/%s", path);
        for (i = 0; i < p->count; i++)
            if (!strcmp(p->files[i].path, counterpart))
                return 1;
        return 0;
    }
    for (i = 0; i < sizeof(prefixes) / sizeof(prefixes[0]); i++)
        if (!strncmp(path, prefixes[i], strlen(prefixes[i])))
            return 1;
    return 0;
}
static int has_file(const Package *p, const char *name) {
    unsigned i;
    for (i = 0; i < p->count; i++)
        if (!strcmp(p->files[i].path, name))
            return 1;
    return 0;
}
static int dependency(const char **cursor, char name[25], char version[32]) {
    const char *text = *cursor, *end, *at;
    size_t n;
    if (!*text)
        return 0;
    end = strchr(text, ',');
    if (!end)
        end = text + strlen(text);
    at = memchr(text, '@', (size_t)(end - text));
    if (!at || (n = (size_t)(at - text)) > 24 || !n || end - at - 1 > 31 || end == at + 1)
        return pkg_fail("invalid dependency", text);
    memcpy(name, text, n);
    name[n] = 0;
    n = (size_t)(end - at - 1);
    memcpy(version, at + 1, n);
    version[n] = 0;
    if (!pkg_token(name, 24) || (strcmp(version, "*") && !pkg_token(version, 31)))
        return pkg_fail("invalid dependency", text);
    *cursor = *end ? end + 1 : end;
    if (*end && !**cursor)
        return pkg_fail("empty dependency", text);
    return 1;
}
static int dependencies(Store *s, const Package *p) {
    const char *cursor = p->depends;
    char name[25], version[32], path[512];
    Package *other = calloc(1, sizeof(*other));
    int n, failed = 0;
    unsigned count = 0;
    if (!other)
        return pkg_fail("out of memory", "");
    while ((n = dependency(&cursor, name, version)) > 0) {
        if (++count > 16 || !strcmp(name, p->name)) {
            pkg_fail("invalid/self dependency", name);
            failed = 1;
            break;
        }
        if (dbpath(path, s, name, ".db") || pkg_db_read(path, other) ||
            (strcmp(version, "*") && strcmp(version, other->version))) {
            pkg_fail("missing exact dependency", name);
            failed = 1;
            break;
        }
    }
    free(other);
    return failed || n < 0 ? -1 : 0;
}
static int conflicts(Store *s, const Package *p, int removing) {
    DIR *dir = opendir(s->db);
    struct dirent *entry;
    Package *other = calloc(1, sizeof(*other));
    int failed = 0;
    if (!dir || !other) {
        if (dir)
            closedir(dir);
        free(other);
        return pkg_fail("cannot inspect ownership", s->db);
    }
    while ((entry = readdir(dir)) != NULL) {
        char path[512];
        size_t len = strlen(entry->d_name);
        unsigned i;
        if (len < 4 || strcmp(entry->d_name + len - 3, ".db"))
            continue;
        if (pkg_join(path, s->db, entry->d_name) || pkg_db_read(path, other)) {
            failed = 1;
            break;
        }
        if (!strcmp(other->name, p->name))
            continue;
        if (removing) {
            const char *cursor = other->depends;
            char name[25], version[32];
            int n;
            while ((n = dependency(&cursor, name, version)) > 0)
                if (!strcmp(name, p->name)) {
                    pkg_fail("required by installed package", other->name);
                    failed = 1;
                    break;
                }
            if (n < 0)
                failed = 1;
        } else {
            const char *cursor = other->depends;
            char name[25], version[32];
            int n;
            while ((n = dependency(&cursor, name, version)) > 0)
                if (!strcmp(name, p->name) && strcmp(version, "*") && strcmp(version, p->version)) {
                    pkg_fail("version required by installed package", other->name);
                    failed = 1;
                    break;
                }
            if (n < 0)
                failed = 1;
        }
        if (!removing && !failed)
            for (i = 0; i < p->count; i++)
                if (has_file(other, p->files[i].path)) {
                    pkg_fail("path owned by another package", p->files[i].path);
                    failed = 1;
                    break;
                }
        if (failed)
            break;
    }
    closedir(dir);
    free(other);
    return failed ? -1 : 0;
}
static int link_text(char out[512], const Package *p, const char *path) {
    const char *at = path;
    char id[25];
    out[0] = 0;
    while (*at)
        if (*at++ == '/')
            strcat(out, "../");
    strcat(out, "var/lib/pkg/objects/");
    memcpy(id, p->hash, 24);
    id[24] = 0;
    strcat(out, id);
    strcat(out, "/root/");
    if (strlen(out) + strlen(path) >= 512)
        return pkg_fail("link too long", path);
    strcat(out, path);
    return 0;
}
static int matching_link(const char *dest, const Package *p, const char *path) {
    char actual[512], expected[512];
    ssize_t n = readlink(dest, actual, 511);
    if (n < 0 || n >= 511 || link_text(expected, p, path))
        return 0;
    actual[n] = 0;
    return !strcmp(actual, expected);
}
static int seed_matches(const char *, const PkgFile *);
static int check_destination(Store *s, const Package *p, const Package *old) {
    unsigned i;
    char dest[512];
    struct stat st;
    for (i = 0; i < p->count; i++) {
        const char *path = p->files[i].path;
        if (!permitted(p, path) || pkg_join(dest, s->root, path))
            return pkg_fail("unsupported install path", path);
        if (!strncmp(path, "etc/", 4))
            continue;
        if (lstat(dest, &st) == 0) {
            unsigned j;
            if (old && !strcmp(old->version, "seed")) {
                for (j = 0; j < old->count; j++)
                    if (!strcmp(old->files[j].path, path) && seed_matches(dest, &old->files[j]))
                        break;
                if (j < old->count)
                    continue;
            }
            if (!S_ISLNK(st.st_mode) ||
                (!matching_link(dest, p, path) &&
                 (!old || !has_file(old, path) || !matching_link(dest, old, path))))
                return pkg_fail("unmanaged/modified destination", dest);
        } else if (errno != ENOENT)
            return pkg_fail("cannot inspect destination", dest);
    }
    return 0;
}
static int verify_object(Store *s, const Package *p) {
    unsigned i;
    char base[512], path[512], hash[65];
    unsigned long size;
    struct stat st;
    if (object_path(base, s, p, "root"))
        return -1;
    for (i = 0; i < p->count; i++)
        if (pkg_join(path, base, p->files[i].path) || lstat(path, &st) || !S_ISREG(st.st_mode) ||
            (st.st_mode & 0777) != p->files[i].mode || pkg_hash_file(path, hash, &size) ||
            size != p->files[i].size || strcmp(hash, p->files[i].hash))
            return pkg_fail("missing/modified object payload", p->files[i].path);
    return 0;
}
#include "seed.inc"
static int install_links(Store *s, const Package *p, const Package *old) {
    unsigned i;
    char dest[512], parent[512], target[512], temp[512];
    if (check_destination(s, p, old) || verify_object(s, p))
        return -1;
    for (i = 0; i < p->count; i++) {
        char *slash;
        pkg_join(dest, s->root, p->files[i].path);
        strcpy(parent, dest);
        slash = strrchr(parent, '/');
        if (!slash)
            return pkg_fail("invalid destination", dest);
        *slash = 0;
        if (!strncmp(p->files[i].path, "etc/", 4)) {
            if (install_config(s, p, old, i))
                return -1;
            continue;
        }
        if (pkg_mkdirs(parent) || link_text(target, p, p->files[i].path))
            return -1;
        if (matching_link(dest, p, p->files[i].path))
            continue;
        {
            char name[40];
            snprintf(name, sizeof(name), ".pkg-link-%ld", (long)getpid());
            if (pkg_join(temp, parent, name))
                return -1;
        }
        if (symlink(target, temp))
            return pkg_fail("cannot stage owned link", temp);
        if (rename(temp, dest)) {
            unlink(temp);
            return pkg_fail("cannot replace owned link", dest);
        }
    }
    return 0;
}
static int check_removal(Store *s, const Package *old, const Package *replacement) {
    unsigned i;
    char path[512];
    for (i = 0; i < old->count; i++) {
        if (!strncmp(old->files[i].path, "etc/", 4))
            continue;
        if (replacement && has_file(replacement, old->files[i].path))
            continue;
        if (pkg_join(path, s->root, old->files[i].path))
            return -1;
        if (!exists(path))
            continue;
        if (!matching_link(path, old, old->files[i].path))
            return pkg_fail("preserving modified destination", path);
    }
    return 0;
}
static int remove_links(Store *s, const Package *old, const Package *replacement) {
    unsigned i;
    char path[512];
    if (check_removal(s, old, replacement))
        return -1;
    for (i = 0; i < old->count; i++) {
        if (!strncmp(old->files[i].path, "etc/", 4))
            continue;
        if (replacement && has_file(replacement, old->files[i].path))
            continue;
        if (pkg_join(path, s->root, old->files[i].path))
            return -1;
        if (unlink(path) && errno != ENOENT)
            return pkg_fail("cannot unlink owned path", path);
    }
    return 0;
}
static int mark_ready(Store *s) {
    char path[512];
    FILE *f;
    if (pkg_join(path, s->txn, "ready"))
        return -1;
    f = fopen(path, "w");
    if (!f)
        return pkg_fail("cannot prepare transaction", path);
    return fclose(f) ? pkg_fail("cannot prepare transaction", path) : 0;
}
static void checkpoint(const char *point) {
#ifdef PKG_TESTING
    const char *stop = getenv("PKG_TEST_STOP");
    if (stop && !strcmp(stop, point))
        _exit(99);
#else
    (void)point;
#endif
}
static int commit_record(Store *s, const char *source, const char *dest) {
    char temp[512];
    if (pkg_join(temp, s->db, ".record.new") || pkg_copy(source, temp, 0600))
        return -1;
    if (rename(temp, dest))
        return pkg_fail("cannot commit package record", dest);
    return 0;
}
static int finish_transaction(Store *s) {
    char path[512], current[512], previous[512];
    Package *p = calloc(1, sizeof(*p)), *old = calloc(1, sizeof(*old));
    int removing, had_old, failed = 1;
    if (!p || !old) {
        pkg_fail("out of memory", "");
        goto done;
    }
    pkg_join(path, s->txn, "ready");
    if (!exists(path)) {
        failed = pkg_remove_tree(s->txn) != 0;
        goto done;
    }
    pkg_join(path, s->txn, "remove");
    removing = exists(path);
    pkg_join(path, s->txn, "old");
    had_old = exists(path);
    if (had_old && pkg_db_read(path, old))
        goto done;
    if (removing) {
        if (!had_old || conflicts(s, old, 1) || remove_links(s, old, NULL))
            goto done;
        if (dbpath(current, s, old->name, ".db") || dbpath(previous, s, old->name, ".prev") ||
            commit_record(s, path, previous))
            goto done;
        if (unlink(current) && errno != ENOENT) {
            pkg_fail("cannot remove package record", current);
            goto done;
        }
    } else {
        pkg_join(path, s->txn, "new");
        if (pkg_db_read(path, p) || dependencies(s, p) || conflicts(s, p, 0) ||
            (had_old && check_removal(s, old, p)) || install_links(s, p, had_old ? old : NULL))
            goto done;
        checkpoint("linked");
        if (had_old && remove_links(s, old, p))
            goto done;
        if (dbpath(current, s, p->name, ".db"))
            goto done;
        if (had_old) {
            pkg_join(path, s->txn, "old");
            if (dbpath(previous, s, p->name, ".prev") || commit_record(s, path, previous))
                goto done;
        }
        pkg_join(path, s->txn, "new");
        if (commit_record(s, path, current))
            goto done;
    }
    checkpoint("committed");
    failed = pkg_remove_tree(s->txn) != 0;
done:
    free(p);
    free(old);
    return failed ? -1 : 0;
}
int pkg_recover(const char *root) {
    Store *s = open_store(root);
    char stage[512];
    int result;
    if (!s)
        return -1;
    result = exists(s->txn) ? finish_transaction(s) : 0;
    /* Extraction precedes journal preparation. A killed extractor can leave
     * this private staging directory even when there is no transaction yet. */
    if (!result && (pkg_join(stage, s->base, "stage") || pkg_remove_tree(stage)))
        result = -1;
    if (!result && (pkg_join(stage, s->base, "stage-seed") || pkg_remove_tree(stage)))
        result = -1;
    close_store(s);
    return result;
}
int pkg_install(const char *root, const char *target, const char *archive) {
    Store *s = open_store(root);
    Package *p = calloc(1, sizeof(*p)), *old = calloc(1, sizeof(*old));
    char path[512], object[512], staging[512], payload[512], digest[65];
    int failed = 1, had_old = 0, staged = 0;
    if (!s || !p || !old) {
        pkg_fail("cannot initialize package manager", root);
        goto done;
    }
    if (exists(s->txn)) {
        pkg_fail("unfinished transaction; run pkg recover", s->txn);
        goto done;
    }
    if (pkg_inspect(archive, p, NULL))
        goto done;
    if (strcmp(p->target, target) && strcmp(p->target, "any")) {
        pkg_fail("package target mismatch", p->target);
        goto done;
    }
    if (dbpath(path, s, p->name, ".db"))
        goto done;
    had_old = exists(path);
    if (had_old && pkg_db_read(path, old))
        goto done;
    if (dependencies(s, p) || conflicts(s, p, 0))
        goto done;
    if (!had_old && pkg_adopt_seed) {
        if (capture_seed(s, p, old))
            goto done;
        had_old = old->count != 0;
    }
    if (check_destination(s, p, had_old ? old : NULL))
        goto done;
    if (had_old && verify_object(s, old))
        goto done;
    if (object_path(object, s, p, ""))
        goto done;
    object[strlen(object) - 1] = 0;
    if (exists(object)) {
        if (object_path(path, s, p, "identity"))
            goto done;
        {
            FILE *f = fopen(path, "r");
            if (!f || !fgets(digest, sizeof(digest), f) || strcmp(digest, p->hash) ||
                fgetc(f) != EOF) {
                if (f)
                    fclose(f);
                pkg_fail("object identity collision", object);
                goto done;
            }
            fclose(f);
        }
        if (verify_object(s, p))
            goto done;
    } else {
        if (pkg_join(staging, s->base, "stage") || exists(staging) || mkdir(staging, 0700)) {
            pkg_fail("staging directory unavailable", s->base);
            goto done;
        }
        staged = 1;
        if (pkg_join(payload, staging, "root") || pkg_mkdirs(payload) ||
            pkg_inspect(archive, p, payload))
            goto done;
        if (pkg_join(path, staging, "identity"))
            goto done;
        {
            FILE *f = fopen(path, "w");
            if (!f) {
                pkg_fail("cannot write object identity", path);
                goto done;
            }
            int bad = fputs(p->hash, f) < 0;
            if (fclose(f))
                bad = 1;
            if (bad) {
                pkg_fail("cannot save object identity", path);
                goto done;
            }
        }
        if (rename(staging, object)) {
            pkg_fail("cannot commit staged payload", object);
            goto done;
        }
        staged = 0;
    }
    if (mkdir(s->txn, 0700)) {
        pkg_fail("cannot create transaction", s->txn);
        goto done;
    }
    if (had_old) {
        pkg_join(path, s->txn, "old");
        if (pkg_db_write(path, old))
            goto done;
    }
    pkg_join(path, s->txn, "new");
    if (pkg_db_write(path, p))
        goto done;
    if (mark_ready(s))
        goto done;
    checkpoint("prepared");
    failed = finish_transaction(s) != 0;
done:
    if (staged)
        pkg_remove_tree(staging);
    free(p);
    free(old);
    close_store(s);
    return failed ? -1 : 0;
}
static int remove_locked(Store *s, const char *name) {
    Package *p = calloc(1, sizeof(*p));
    char path[512], source[512];
    int failed = 1;
    if (!s || !p)
        goto done;
    if (exists(s->txn)) {
        pkg_fail("unfinished transaction; run pkg recover", s->txn);
        goto done;
    }
    if (dbpath(source, s, name, ".db") || pkg_db_read(source, p) || conflicts(s, p, 1) ||
        verify_object(s, p))
        goto done;
    if (mkdir(s->txn, 0700)) {
        pkg_fail("cannot create transaction", s->txn);
        goto done;
    }
    pkg_join(path, s->txn, "old");
    if (pkg_copy(source, path, 0600))
        goto done;
    pkg_join(path, s->txn, "remove");
    {
        FILE *f = fopen(path, "w");
        if (!f)
            goto done;
        if (fclose(f))
            goto done;
    }
    if (mark_ready(s))
        goto done;
    checkpoint("prepared");
    failed = finish_transaction(s) != 0;
done:
    free(p);
    return failed ? -1 : 0;
}
int pkg_remove(const char *root, const char *name) {
    Store *s = open_store(root);
    int result = s ? remove_locked(s, name) : -1;
    close_store(s);
    return result;
}
int pkg_query(const char *root, const char *name) {
    Store *s = open_store(root);
    Package *p = calloc(1, sizeof(*p));
    DIR *dir = NULL;
    struct dirent *entry;
    char path[512];
    int failed = 1;
    if (!s || !p)
        goto done;
    if (name) {
        unsigned i;
        if (dbpath(path, s, name, ".db") || pkg_db_read(path, p))
            goto done;
        printf("%s %s [%s]\ndepends=%s\n", p->name, p->version, p->target, p->depends);
        for (i = 0; i < p->count; i++)
            printf("/%s\n", p->files[i].path);
    } else {
        dir = opendir(s->db);
        if (!dir)
            goto done;
        while ((entry = readdir(dir)) != NULL) {
            size_t n = strlen(entry->d_name);
            if (n < 4 || strcmp(entry->d_name + n - 3, ".db"))
                continue;
            if (pkg_join(path, s->db, entry->d_name) || pkg_db_read(path, p))
                goto done;
            printf("%s %s [%s]\n", p->name, p->version, p->target);
        }
    }
    failed = 0;
done:
    if (dir)
        closedir(dir);
    free(p);
    close_store(s);
    return failed ? -1 : 0;
}
/* Content-addressed support packages are implementation details. Retain any
 * version referenced by a current or retained previous package record. */
static int automatic(const char *name) {
    const char *hash;
    if (!strncmp(name, "fxlib-", 6))
        hash = name + 6;
    else if (!strncmp(name, "bundle-", 7))
        hash = name + 7;
    else
        return 0;
    return strlen(hash) == 16 && strspn(hash, "0123456789abcdef") == 16;
}
static int referenced_package(Store *s, const char *name) {
    DIR *dir = opendir(s->db);
    struct dirent *entry;
    Package *p = calloc(1, sizeof(*p));
    int result = -1;
    if (!dir || !p)
        goto done;
    result = 0;
    while ((entry = readdir(dir)) != NULL) {
        size_t n = strlen(entry->d_name);
        char path[512], dep[25], version[32];
        const char *cursor;
        int next;
        if (!((n > 3 && !strcmp(entry->d_name + n - 3, ".db")) ||
              (n > 5 && !strcmp(entry->d_name + n - 5, ".prev"))))
            continue;
        if (pkg_join(path, s->db, entry->d_name) || pkg_db_read(path, p)) {
            result = -1;
            break;
        }
        cursor = p->depends;
        while ((next = dependency(&cursor, dep, version)) > 0) {
            if (!strcmp(dep, name)) {
                result = 1;
                break;
            }
        }
        if (next < 0)
            result = -1;
        if (result)
            break;
    }
done:
    if (dir)
        closedir(dir);
    free(p);
    return result;
}
static int clean_support(Store *s) {
    for (;;) {
        DIR *dir = opendir(s->db);
        struct dirent *entry;
        char orphan[25] = "", path[512];
        int failed = 0;
        if (!dir)
            return -1;
        while ((entry = readdir(dir)) != NULL) {
            size_t n = strlen(entry->d_name);
            char name[25];
            int referenced;
            if (n <= 3 || n > 27 || strcmp(entry->d_name + n - 3, ".db"))
                continue;
            memcpy(name, entry->d_name, n - 3);
            name[n - 3] = 0;
            if (!automatic(name))
                continue;
            referenced = referenced_package(s, name);
            if (referenced < 0) {
                failed = 1;
                break;
            }
            if (!referenced) {
                strcpy(orphan, name);
                break;
            }
        }
        closedir(dir);
        if (failed)
            return -1;
        if (!*orphan)
            return 0;
        if (remove_locked(s, orphan) || dbpath(path, s, orphan, ".prev") || unlink(path))
            return -1;
        printf("Removed unused support package %s\n", orphan);
    }
}
int pkg_clean(const char *root) {
    Store *s = open_store(root);
    DIR *objects = NULL, *db = NULL;
    struct dirent *entry;
    Package *p = calloc(1, sizeof(*p));
    int failed = 1;
    if (!s || !p)
        goto done;
    if (exists(s->txn)) {
        pkg_fail("recover transaction before cleaning", s->txn);
        goto done;
    }
    /* Retain one previous version for installed packages. After uninstall,
     * explicit clean releases that package's remaining payload as well. */
    db = opendir(s->db);
    if (!db)
        goto done;
    while ((entry = readdir(db)) != NULL) {
        size_t n = strlen(entry->d_name);
        char path[512], current[512];
        if (n <= 5 || strcmp(entry->d_name + n - 5, ".prev"))
            continue;
        if (pkg_join(path, s->db, entry->d_name) || pkg_db_read(path, p) ||
            dbpath(current, s, p->name, ".db"))
            goto done;
        if ((!exists(current) || pkg_discard_previous) && unlink(path)) {
            pkg_fail("cannot release old record", path);
            goto done;
        }
    }
    closedir(db);
    db = NULL;
    if (clean_support(s))
        goto done;
    objects = opendir(s->objects);
    if (!objects)
        goto done;
    while ((entry = readdir(objects)) != NULL) {
        char id[25], path[512];
        int referenced = 0;
        struct dirent *item;
        if (strlen(entry->d_name) != 24 || strspn(entry->d_name, "0123456789abcdef") != 24)
            continue;
        strcpy(id, entry->d_name);
        db = opendir(s->db);
        if (!db)
            goto done;
        while ((item = readdir(db)) != NULL) {
            size_t n = strlen(item->d_name);
            if ((n > 3 && !strcmp(item->d_name + n - 3, ".db")) ||
                (n > 5 && !strcmp(item->d_name + n - 5, ".prev"))) {
                if (pkg_join(path, s->db, item->d_name) || pkg_db_read(path, p))
                    goto done;
                if (!strncmp(p->hash, id, 24)) {
                    referenced = 1;
                    break;
                }
            }
        }
        closedir(db);
        db = NULL;
        if (!referenced) {
            if (pkg_join(path, s->objects, id) || pkg_remove_tree(path))
                goto done;
        }
    }
    failed = 0;
done:
    if (objects)
        closedir(objects);
    if (db)
        closedir(db);
    free(p);
    close_store(s);
    return failed ? -1 : 0;
}
