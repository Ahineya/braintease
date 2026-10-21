/* Imported seed files are snapshotted before their managed links replace them.
 * Only --adopt-seed enables this path; configuration files retain their own
 * copy/preserve policy. The package database lock covers the whole operation. */
static int seed_matches(const char *path, const PkgFile *item) {
    struct stat st;
    char hash[65];
    unsigned long size;
    if (lstat(path, &st))
        return 0;
    if (S_ISREG(st.st_mode))
        return (st.st_mode & 0777) == item->mode && !pkg_hash_file(path, hash, &size) &&
               size == item->size && !strcmp(hash, item->hash);
    if (S_ISLNK(st.st_mode)) {
        char text[256];
        ssize_t n = readlink(path, text, 255);
        PkgSha sha;
        if (n <= 0)
            return 0;
        text[n] = 0;
        if (!pkg_token(text, 30) || !strcmp(text, ".") || !strcmp(text, ".."))
            return 0;
        pkg_sha_init(&sha);
        pkg_sha_update(&sha, (PkgByte *)text, (size_t)n);
        pkg_sha_end(&sha, hash);
        return (unsigned long)n == item->size && !strcmp(hash, item->hash);
    }
    return 0;
}
static int capture_seed(Store *s, const Package *p, Package *old) {
    char stage[512], root[512], source[512], dest[512], record[512], object[512];
    unsigned i;
    int result = -1;
    strcpy(old->name, p->name);
    strcpy(old->version, "seed");
    strcpy(old->target, p->target);
    if (pkg_join(stage, s->base, "stage-seed") || exists(stage) || mkdir(stage, 0700))
        return pkg_fail("seed staging unavailable; run pkg recover", s->base);
    if (pkg_join(root, stage, "root") || pkg_mkdirs(root))
        goto done;
    for (i = 0; i < p->count; i++) {
        PkgFile *item;
        struct stat st;
        char parent[512], *slash;
        if (!strncmp(p->files[i].path, "etc/", 4))
            continue;
        if (pkg_join(source, s->root, p->files[i].path))
            goto done;
        if (lstat(source, &st)) {
            if (errno == ENOENT)
                continue;
            pkg_fail("cannot inspect seed", source);
            goto done;
        }
        item = &old->files[old->count++];
        strcpy(item->path, p->files[i].path);
        if (pkg_join(dest, root, item->path))
            goto done;
        if (S_ISREG(st.st_mode)) {
            item->mode = st.st_mode & 0777;
            if (pkg_hash_file(source, item->hash, &item->size) ||
                pkg_copy(source, dest, item->mode))
                goto done;
        } else if (S_ISLNK(st.st_mode) && !strncmp(item->path, "bin/", 4)) {
            char text[256];
            ssize_t n = readlink(source, text, 255);
            FILE *f;
            PkgSha sha;
            int bad;
            if (n <= 0)
                goto done;
            text[n] = 0;
            if (!pkg_token(text, 30) || !strcmp(text, ".") || !strcmp(text, "..")) {
                pkg_fail("unsupported seed alias", source);
                goto done;
            }
            item->mode = 0777;
            item->size = (unsigned long)n;
            pkg_sha_init(&sha);
            pkg_sha_update(&sha, (PkgByte *)text, (size_t)n);
            pkg_sha_end(&sha, item->hash);
            strcpy(parent, dest);
            slash = strrchr(parent, '/');
            *slash = 0;
            if (pkg_mkdirs(parent))
                goto done;
            f = fopen(dest, "wb");
            if (!f)
                goto done;
            bad = fwrite(text, 1, (size_t)n, f) != (size_t)n;
            if (fclose(f))
                bad = 1;
            if (bad || chmod(dest, 0777))
                goto done;
        } else {
            pkg_fail("refusing nonregular seed", source);
            goto done;
        }
        if (!seed_matches(source, item)) {
            pkg_fail("seed changed while snapshotting", source);
            goto done;
        }
    }
    if (!old->count) {
        result = 0;
        goto done;
    }
    if (pkg_join(record, stage, "record") || pkg_db_write(record, old) ||
        pkg_hash_file(record, old->hash, NULL) || unlink(record))
        goto done;
    if (pkg_join(record, stage, "identity"))
        goto done;
    {
        FILE *f = fopen(record, "w");
        int bad;
        if (!f)
            goto done;
        bad = fputs(old->hash, f) < 0;
        if (fclose(f))
            bad = 1;
        if (bad)
            goto done;
    }
    if (object_path(object, s, old, ""))
        goto done;
    object[strlen(object) - 1] = 0;
    if (exists(object)) {
        char identity[65];
        FILE *f;
        if (object_path(record, s, old, "identity"))
            goto done;
        f = fopen(record, "r");
        if (!f)
            goto done;
        if (!fgets(identity, 65, f) || strcmp(identity, old->hash) || fgetc(f) != EOF) {
            fclose(f);
            pkg_fail("seed identity collision", object);
            goto done;
        }
        fclose(f);
    } else if (rename(stage, object)) {
        pkg_fail("cannot retain original seed", object);
        goto done;
    }
    if (verify_object(s, old))
        goto done;
    result = 0;
done:
    pkg_remove_tree(stage);
    return result;
}
static int install_config(Store *s, const Package *p, const Package *old, unsigned at) {
    char dest[512], source[512], base[512], temp[512], hash[65];
    struct stat st;
    unsigned i;
    unsigned long size;
    if (pkg_join(dest, s->root, p->files[at].path))
        return -1;
    {
        char parent[512], *slash;
        strcpy(parent, dest);
        slash = strrchr(parent, '/');
        *slash = 0;
        if (pkg_mkdirs(parent))
            return -1;
    }
    if (lstat(dest, &st) == 0) {
        if (!S_ISREG(st.st_mode) || !old)
            return 0;
        for (i = 0; i < old->count; i++)
            if (!strcmp(old->files[i].path, p->files[at].path))
                break;
        if (i == old->count || pkg_hash_file(dest, hash, &size) || size != old->files[i].size ||
            strcmp(hash, old->files[i].hash))
            return 0;
    } else if (errno != ENOENT)
        return pkg_fail("cannot inspect configuration", dest);
    if (object_path(base, s, p, "root") || pkg_join(source, base, p->files[at].path))
        return -1;
    if (pkg_join(temp, s->txn, "config-new"))
        return -1;
    if (lstat(temp, &st) == 0 && unlink(temp))
        return pkg_fail("cannot reset configuration staging", temp);
    if (pkg_copy(source, temp, p->files[at].mode))
        return -1;
    if (rename(temp, dest)) {
        unlink(temp);
        return pkg_fail("cannot install configuration", dest);
    }
    return 0;
}
