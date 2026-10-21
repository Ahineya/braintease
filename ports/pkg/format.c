#include "pkg.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

char pkg_error[256];
int pkg_fail(const char *message, const char *path) {
    snprintf(pkg_error, sizeof(pkg_error), "%s: %.160s", message, path ? path : "");
    return -1;
}
int pkg_token(const char *s, size_t max) {
    size_t n = 0;
    if (!s || !*s)
        return 0;
    while (*s) {
        char c = *s++;
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
              c == '-' || c == '_' || c == '.' || c == '+'))
            return 0;
        if (++n > max)
            return 0;
    }
    return 1;
}
int pkg_digest_ok(const char *s) {
    unsigned i;
    if (strlen(s) != 64)
        return 0;
    for (i = 0; i < 64; i++)
        if (!((s[i] >= '0' && s[i] <= '9') || (s[i] >= 'a' && s[i] <= 'f')))
            return 0;
    return 1;
}
static int valid_dependencies(const char *text) {
    unsigned count = 0;
    char copy[256], *part, *next, *at;
    if (strlen(text) >= sizeof(copy))
        return 0;
    strcpy(copy, text);
    part = copy;
    while (*part) {
        if (++count > 16)
            return 0;
        next = strchr(part, ',');
        if (next)
            *next++ = 0;
        at = strchr(part, '@');
        if (!at)
            return 0;
        *at++ = 0;
        if (!pkg_token(part, 24) || !strcmp(part, ".") || !strcmp(part, "..") ||
            (strcmp(at, "*") && !pkg_token(at, 31)))
            return 0;
        if (!next)
            break;
        if (!*next)
            return 0;
        part = next;
    }
    return 1;
}
int pkg_path_ok(const char *s) {
    const char *part = s;
    size_t n = 0;
    if (!*s || *s == '/')
        return 0;
    for (;; s++) {
        if (!*s || *s == '/') {
            size_t len = (size_t)(s - part);
            if (!len || len > 30 || (len == 1 && part[0] == '.') ||
                (len == 2 && part[0] == '.' && part[1] == '.'))
                return 0;
            if (!*s)
                return n < PKG_PATH;
            part = s + 1;
        } else if ((unsigned char)*s < 32U || (unsigned char)*s > 126U || *s == ',' || *s == '\\')
            return 0;
        if (++n >= PKG_PATH)
            return 0;
    }
}
int pkg_join(char out[512], const char *a, const char *b) {
    if (strlen(a) + strlen(b) + 2 > 512)
        return pkg_fail("path too long", b);
    strcpy(out, a);
    if (*a && a[strlen(a) - 1] != '/')
        strcat(out, "/");
    strcat(out, b);
    return 0;
}
int pkg_mkdirs(const char *path) {
    char copy[512];
    size_t i;
    struct stat st;
    if (strlen(path) >= sizeof(copy))
        return pkg_fail("directory too long", path);
    if (!*path)
        return pkg_fail("empty directory", path);
    strcpy(copy, path);
    for (i = 1;; i++)
        if (copy[i] == '/' || copy[i] == 0) {
            char saved = copy[i];
            copy[i] = 0;
            if (lstat(copy, &st) == 0) {
                if (!S_ISDIR(st.st_mode))
                    return pkg_fail("directory is not a real directory", copy);
            } else if (errno != ENOENT || mkdir(copy, 0755))
                return pkg_fail("cannot create directory", copy);
            copy[i] = saved;
            if (!saved)
                break;
        }
    return 0;
}
static int parents(const char *path) {
    char copy[512], *slash;
    if (strlen(path) >= sizeof(copy))
        return pkg_fail("path too long", path);
    strcpy(copy, path);
    slash = strrchr(copy, '/');
    if (!slash || slash == copy)
        return 0;
    *slash = 0;
    return pkg_mkdirs(copy);
}
int pkg_copy(const char *from, const char *to, unsigned mode) {
    PkgByte buffer[2048];
    size_t n;
    int failed = 0;
    FILE *in = fopen(from, "rb"), *out;
    if (!in)
        return pkg_fail("cannot read", from);
    if (parents(to)) {
        fclose(in);
        return -1;
    }
    out = fopen(to, "wb");
    if (!out) {
        fclose(in);
        return pkg_fail("cannot create", to);
    }
    while ((n = fread(buffer, 1, 2048, in)) != 0)
        if (fwrite(buffer, 1, n, out) != n) {
            failed = 1;
            break;
        }
    if (ferror(in))
        failed = 1;
    if (fclose(in))
        failed = 1;
    if (fclose(out))
        failed = 1;
    if (failed || chmod(to, mode)) {
        unlink(to);
        return pkg_fail("copy failed", to);
    }
    return 0;
}
int pkg_remove_tree(const char *path) {
    struct stat st;
    DIR *dir;
    struct dirent *entry;
    char child[512];
    int failed = 0;
    if (lstat(path, &st)) {
        if (errno == ENOENT)
            return 0;
        return pkg_fail("cannot stat", path);
    }
    if (!S_ISDIR(st.st_mode))
        return unlink(path) ? pkg_fail("cannot unlink", path) : 0;
    dir = opendir(path);
    if (!dir)
        return pkg_fail("cannot open directory", path);
    while ((entry = readdir(dir)) != NULL) {
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, ".."))
            continue;
        if (pkg_join(child, path, entry->d_name) || pkg_remove_tree(child)) {
            failed = 1;
            break;
        }
    }
    closedir(dir);
    if (failed)
        return -1;
    return rmdir(path) ? pkg_fail("cannot remove directory", path) : 0;
}
static int line(FILE *f, char out[512]) {
    size_t n;
    if (!fgets(out, 512, f))
        return pkg_fail("truncated metadata", "");
    n = strlen(out);
    if (!n || out[n - 1] != '\n')
        return pkg_fail("metadata line too long", "");
    out[n - 1] = 0;
    return 0;
}
static int field(FILE *f, const char *key, char *out, size_t max) {
    char text[512];
    size_t n = strlen(key);
    if (line(f, text))
        return -1;
    if (strncmp(text, key, n) || strlen(text + n) >= max)
        return pkg_fail("invalid metadata field", key);
    strcpy(out, text + n);
    return 0;
}
int pkg_write_meta(FILE *f, const Package *p, const char *magic) {
    if (fprintf(f, "%s\nname=%s\nversion=%s\ntarget=%s\ndepends=%s\nfiles=%u\n", magic, p->name,
                p->version, p->target, p->depends, p->count) < 0)
        return -1;
    return 0;
}
int pkg_read_meta(FILE *f, Package *p, const char *magic) {
    char text[512], *end;
    unsigned long n;
    if (line(f, text) || strcmp(text, magic))
        return pkg_fail("invalid package magic", "");
    if (field(f, "name=", p->name, sizeof(p->name)) ||
        field(f, "version=", p->version, sizeof(p->version)) ||
        field(f, "target=", p->target, sizeof(p->target)) ||
        field(f, "depends=", p->depends, sizeof(p->depends)) || line(f, text))
        return -1;
    if (!pkg_token(p->name, 24) || !strcmp(p->name, ".") || !strcmp(p->name, "..") ||
        !pkg_token(p->version, 31) || !pkg_token(p->target, 63) || !valid_dependencies(p->depends))
        return pkg_fail("invalid package identity", p->name);
    if (strncmp(text, "files=", 6) || text[6] < '0' || text[6] > '9')
        return pkg_fail("missing file count", "");
    n = strtoul(text + 6, &end, 10);
    if (*end || n > PKG_FILES)
        return pkg_fail("invalid file count", "");
    p->count = (unsigned)n;
    return 0;
}
static int record(FILE *f, PkgFile *item) {
    char text[512], *mode, *size, *hash, *path, *end;
    unsigned long n;
    if (line(f, text) || strncmp(text, "file=", 5))
        return pkg_fail("missing file record", "");
    mode = text + 5;
    size = strchr(mode, ',');
    if (!size)
        return pkg_fail("bad file record", "");
    *size++ = 0;
    hash = strchr(size, ',');
    if (!hash)
        return pkg_fail("bad file record", "");
    *hash++ = 0;
    path = strchr(hash, ',');
    if (!path)
        return pkg_fail("bad file record", "");
    *path++ = 0;
    n = strtoul(mode, &end, 8);
    if (*mode < '0' || *mode > '7' || *end || n > 0777)
        return pkg_fail("invalid file mode", path);
    item->mode = (unsigned)n;
    if (!*size || *size < '0' || *size > '9')
        return pkg_fail("invalid file length", path);
    n = strtoul(size, &end, 10);
    if (*end || n > PKG_LIMIT)
        return pkg_fail("file too large", path);
    item->size = n;
    if (!pkg_digest_ok(hash) || !pkg_path_ok(path))
        return pkg_fail("invalid file path/hash", path);
    strcpy(item->hash, hash);
    strcpy(item->path, path);
    return 0;
}
static int write_record(FILE *f, const PkgFile *item) {
    return fprintf(f, "file=%o,%lu,%s,%s\n", item->mode, item->size, item->hash, item->path) < 0
               ? -1
               : 0;
}
int pkg_inspect(const char *path, Package *p, const char *extract) {
    FILE *f = fopen(path, "rb");
    unsigned i;
    PkgByte buffer[2048];
    int failed = 0;
    unsigned long total = 0;
    if (!f)
        return pkg_fail("cannot open package", path);
    if (pkg_read_meta(f, p, "RIPKG1")) {
        fclose(f);
        return -1;
    }
    for (i = 0; i < p->count && !failed; i++) {
        PkgFile *item = &p->files[i];
        unsigned long left;
        PkgSha sha;
        char hash[65], dest[512];
        FILE *out = NULL;
        if (record(f, item)) {
            failed = 1;
            break;
        }
        if (item->size > PKG_LIMIT - total) {
            pkg_fail("package payload limit", path);
            failed = 1;
            break;
        }
        total += item->size;
        if (i && strcmp(p->files[i - 1].path, item->path) >= 0) {
            pkg_fail("unordered/duplicate paths", item->path);
            failed = 1;
            break;
        }
        if (extract) {
            if (pkg_join(dest, extract, item->path) || parents(dest)) {
                failed = 1;
                break;
            }
            out = fopen(dest, "wb");
            if (!out) {
                pkg_fail("cannot stage file", dest);
                failed = 1;
                break;
            }
        }
        left = item->size;
        pkg_sha_init(&sha);
        while (left) {
            size_t n = left > 2048 ? 2048 : (size_t)left, j;
            if (fread(buffer, 1, n, f) != n) {
                failed = 1;
                break;
            }
            for (j = 0; j < n; j++)
                if (((unsigned long)buffer[j] & ~255UL) != 0) {
                    failed = 1;
                    break;
                }
            if (failed)
                break;
            pkg_sha_update(&sha, buffer, n);
            if (out && fwrite(buffer, 1, n, out) != n) {
                failed = 1;
                break;
            }
            left -= (unsigned long)n;
        }
        if (out && fclose(out))
            failed = 1;
        pkg_sha_end(&sha, hash);
        if (failed || strcmp(hash, item->hash) || fgetc(f) != '\n') {
            pkg_fail("payload hash/length mismatch", item->path);
            failed = 1;
            break;
        }
        if (extract && chmod(dest, item->mode)) {
            pkg_fail("cannot set file mode", dest);
            failed = 1;
        }
    }
    if (!failed && (fgetc(f) != EOF || ferror(f))) {
        pkg_fail("trailing package data", path);
        failed = 1;
    }
    if (fclose(f))
        failed = 1;
    return failed ? -1 : pkg_hash_file(path, p->hash, NULL);
}
static int gather(Package *p, const char *stage, const char *relative, unsigned depth) {
    char base[512], child[512], full[512];
    DIR *dir;
    struct dirent *entry;
    int failed = 0;
    if (depth > 16 || pkg_join(base, stage, relative))
        return pkg_fail("staging nesting/path limit", relative);
    dir = opendir(base);
    if (!dir)
        return pkg_fail("cannot read staging directory", base);
    while ((entry = readdir(dir)) != NULL) {
        struct stat st;
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, ".."))
            continue;
        if (*relative) {
            if (pkg_join(child, relative, entry->d_name)) {
                failed = 1;
                break;
            }
        } else
            strcpy(child, entry->d_name);
        if (!pkg_path_ok(child) || pkg_join(full, stage, child) || lstat(full, &st)) {
            pkg_fail("invalid staging path", child);
            failed = 1;
            break;
        }
        if (S_ISDIR(st.st_mode)) {
            if (gather(p, stage, child, depth + 1)) {
                failed = 1;
                break;
            }
        } else if (S_ISREG(st.st_mode) && p->count < PKG_FILES) {
            PkgFile *item = &p->files[p->count++];
            strcpy(item->path, child);
            item->mode = st.st_mode & 0777;
            if (pkg_hash_file(full, item->hash, &item->size)) {
                failed = 1;
                break;
            }
        } else {
            pkg_fail("only regular files/directories allowed", child);
            failed = 1;
            break;
        }
    }
    closedir(dir);
    return failed ? -1 : 0;
}
static int compare(const void *a, const void *b) {
    return strcmp(((const PkgFile *)a)->path, ((const PkgFile *)b)->path);
}
/* An output inside staging would become an input on the next invocation.
 * Compare directory identities so relative paths and aliases cannot hide it. */
static int outside_staging(const char *stage, const char *output) {
    struct stat staging, parent, above;
    char path[512], next[512], *slash;
    unsigned depth;
    if (stat(stage, &staging) || !S_ISDIR(staging.st_mode) || strlen(output) >= sizeof(path))
        return pkg_fail("invalid staging/output path", stage);
    strcpy(path, output);
    slash = strrchr(path, '/');
    if (!slash)
        strcpy(path, ".");
    else if (slash == path)
        slash[1] = 0;
    else
        *slash = 0;
    for (depth = 0; depth < 64; depth++) {
        if (stat(path, &parent))
            return pkg_fail("cannot inspect output directory", path);
        if (parent.st_dev == staging.st_dev && parent.st_ino == staging.st_ino)
            return pkg_fail("package output must be outside staging", output);
        if (pkg_join(next, path, "..") || stat(next, &above))
            return pkg_fail("cannot inspect output ancestor", path);
        if (parent.st_dev == above.st_dev && parent.st_ino == above.st_ino)
            return 0;
        strcpy(path, next);
    }
    return pkg_fail("output ancestry limit", output);
}
int pkg_create(const char *manifest, const char *stage, const char *output) {
    Package *p = calloc(1, sizeof(*p));
    FILE *meta, *out = NULL, *in = NULL;
    char temporary[512] = {0}, source[512], text[512];
    int failed = 1;
    unsigned seen = 0, i;
    if (!p)
        return pkg_fail("out of memory", "");
    if (outside_staging(stage, output)) {
        free(p);
        return -1;
    }
    meta = fopen(manifest, "r");
    if (!meta) {
        free(p);
        return pkg_fail("cannot read manifest", manifest);
    }
    while (fgets(text, sizeof(text), meta)) {
        char *value = strchr(text, '=');
        size_t n = strlen(text);
        unsigned bit = 0;
        char *dest = NULL;
        size_t cap = 0;
        if (!n || text[n - 1] != '\n' || !value) {
            pkg_fail("invalid manifest line", manifest);
            goto done_meta;
        }
        text[n - 1] = 0;
        *value++ = 0;
        if (!strcmp(text, "name")) {
            bit = 1;
            dest = p->name;
            cap = sizeof(p->name);
        } else if (!strcmp(text, "version")) {
            bit = 2;
            dest = p->version;
            cap = sizeof(p->version);
        } else if (!strcmp(text, "target")) {
            bit = 4;
            dest = p->target;
            cap = sizeof(p->target);
        } else if (!strcmp(text, "depends")) {
            bit = 8;
            dest = p->depends;
            cap = sizeof(p->depends);
        }
        if (!bit || (seen & bit) || strlen(value) >= cap) {
            pkg_fail("unknown/duplicate/long manifest field", text);
            goto done_meta;
        }
        strcpy(dest, value);
        seen |= bit;
    }
    if (ferror(meta) || (seen & 7) != 7 || !pkg_token(p->name, 24) || !strcmp(p->name, ".") ||
        !strcmp(p->name, "..") || !pkg_token(p->version, 31) || !pkg_token(p->target, 63) ||
        !valid_dependencies(p->depends)) {
        pkg_fail("invalid manifest identity", manifest);
        goto done_meta;
    }
    fclose(meta);
    meta = NULL;
    if (gather(p, stage, "", 0))
        goto done;
    qsort(p->files, p->count, sizeof(p->files[0]), compare);
    {
        unsigned long total = 0;
        for (i = 0; i < p->count; i++) {
            if (p->files[i].size > PKG_LIMIT - total) {
                pkg_fail("package payload limit", stage);
                goto done;
            }
            total += p->files[i].size;
        }
    }
    if (strlen(output) + 32 >= sizeof(temporary)) {
        pkg_fail("output path too long", output);
        goto done;
    }
    strcpy(temporary, output);
    {
        char *slash = strrchr(temporary, '/');
        if (slash)
            slash[1] = 0;
        else
            temporary[0] = 0;
    }
    {
        char suffix[40];
        snprintf(suffix, sizeof(suffix), ".pkg-write-%ld", (long)getpid());
        strcat(temporary, suffix);
    }
    {
        int fd = open(temporary, O_WRONLY | O_CREAT | O_EXCL, 0600);
        if (fd < 0) {
            pkg_fail("cannot create temporary package", temporary);
            goto done;
        }
        out = fdopen(fd, "wb");
        if (!out) {
            close(fd);
            unlink(temporary);
            goto done;
        }
    }
    if (pkg_write_meta(out, p, "RIPKG1"))
        goto done;
    for (i = 0; i < p->count; i++) {
        PkgByte buffer[2048];
        size_t n;
        PkgSha sha;
        char hash[65];
        unsigned long total = 0;
        if (pkg_join(source, stage, p->files[i].path) || write_record(out, &p->files[i]))
            goto done;
        in = fopen(source, "rb");
        if (!in) {
            pkg_fail("cannot reopen source", source);
            goto done;
        }
        pkg_sha_init(&sha);
        while ((n = fread(buffer, 1, 2048, in)) != 0) {
            if (fwrite(buffer, 1, n, out) != n)
                goto done;
            pkg_sha_update(&sha, buffer, n);
            total += (unsigned long)n;
        }
        if (ferror(in))
            goto done;
        fclose(in);
        in = NULL;
        pkg_sha_end(&sha, hash);
        if (total != p->files[i].size || strcmp(hash, p->files[i].hash)) {
            pkg_fail("source changed during packaging", source);
            goto done;
        }
        if (fputc('\n', out) == EOF)
            goto done;
    }
    if (fclose(out)) {
        out = NULL;
        goto done;
    }
    out = NULL;
    if (rename(temporary, output)) {
        pkg_fail("cannot commit package", output);
        goto done;
    }
    failed = 0;
done:
    if (in)
        fclose(in);
    if (out)
        fclose(out);
    if (failed && temporary[0])
        unlink(temporary);
    free(p);
    if (failed && !pkg_error[0])
        pkg_fail("package creation failed", output);
    return failed ? -1 : 0;
done_meta:
    fclose(meta);
    free(p);
    return -1;
}
/* Database records have the same bounded metadata/records without payload. */
int pkg_db_write(const char *path, const Package *p) {
    FILE *f = fopen(path, "w");
    unsigned i;
    int failed = 0;
    if (!f)
        return pkg_fail("cannot write metadata", path);
    if (pkg_write_meta(f, p, "RIPDB1") || fprintf(f, "hash=%s\n", p->hash) < 0)
        failed = 1;
    for (i = 0; i < p->count && !failed; i++)
        if (write_record(f, &p->files[i]))
            failed = 1;
    if (fclose(f))
        failed = 1;
    return failed ? pkg_fail("metadata write failed", path) : 0;
}
int pkg_db_read(const char *path, Package *p) {
    FILE *f = fopen(path, "r");
    unsigned i;
    int failed = 0;
    if (!f)
        return pkg_fail("cannot read metadata", path);
    if (pkg_read_meta(f, p, "RIPDB1") || field(f, "hash=", p->hash, sizeof(p->hash)) ||
        !pkg_digest_ok(p->hash))
        failed = 1;
    for (i = 0; i < p->count && !failed; i++)
        if (record(f, &p->files[i]) || (i && strcmp(p->files[i - 1].path, p->files[i].path) >= 0))
            failed = 1;
    if (!failed && fgetc(f) != EOF)
        failed = 1;
    fclose(f);
    return failed ? pkg_fail("invalid database", path) : 0;
}
