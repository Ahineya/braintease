#include "pkg.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/file.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

typedef struct {
    char url[256], cache[512], key[512], index[512], signature[512];
    int lock;
} Repository;
typedef struct {
    char name[25], version[32], target[64], hash[65], path[64], depends[256];
    unsigned long size;
} Entry;
static int ready(int fd, int writing) {
    fd_set set;
    struct timeval timeout;
    int n;
    FD_ZERO(&set);
    FD_SET(fd, &set);
    timeout.tv_sec = 30;
    timeout.tv_usec = 0;
    do {
        n = select(fd + 1, writing ? NULL : &set, writing ? &set : NULL, NULL, &timeout);
    } while (n < 0 && errno == EINTR);
    return n > 0 ? 0 : pkg_fail("repository connection timed out", "");
}
static int receive(int fd, void *out, unsigned n) {
    ssize_t result;
    if (ready(fd, 0))
        return -1;
    do {
        result = recv(fd, out, n, 0);
    } while (result < 0 && errno == EINTR);
    if (result <= 0)
        return pkg_fail("repository response truncated", "");
    return (int)result;
}
static int http_line(int fd, char out[512]) {
    unsigned n = 0;
    char c;
    while (n < 511) {
        if (receive(fd, &c, 1) != 1)
            return -1;
        if (c == '\n') {
            if (n && out[n - 1] == '\r')
                n--;
            out[n] = 0;
            return 0;
        }
        out[n++] = c;
    }
    return pkg_fail("HTTP header too long", "");
}
#ifndef RIPPLE_MMU
/* Native hosts may advertise several mDNS interfaces. getaddrinfo includes
 * scoped IPv6 addresses and avoids gethostbyname's single stale IPv4 choice. */
static int host_connect(const char *host, unsigned port) {
    struct addrinfo hints, *addresses = NULL, *at;
    char service[8];
    int fd = -1;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    snprintf(service, sizeof(service), "%u", port);
    if (getaddrinfo(host, service, &hints, &addresses))
        return -1;
    for (at = addresses; at; at = at->ai_next) {
        int flags, result, error = 0;
        socklen_t size = sizeof(error);
        fd = socket(at->ai_family, at->ai_socktype, at->ai_protocol);
        if (fd < 0)
            continue;
        flags = fcntl(fd, F_GETFL, 0);
        if (fd >= FD_SETSIZE || flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK)) {
            close(fd);
            fd = -1;
            continue;
        }
        result = connect(fd, at->ai_addr, at->ai_addrlen);
        if (result < 0 && errno == EINPROGRESS) {
            fd_set writes;
            struct timeval timeout;
            FD_ZERO(&writes);
            FD_SET(fd, &writes);
            timeout.tv_sec = 3;
            timeout.tv_usec = 0;
            if (select(fd + 1, NULL, &writes, NULL, &timeout) > 0 &&
                !getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &size) && !error)
                result = 0;
        }
        if (!result && !fcntl(fd, F_SETFL, flags))
            break;
        close(fd);
        fd = -1;
    }
    freeaddrinfo(addresses);
    return fd;
}
#endif
/* Bounded static-file HTTP, authenticated at the index/object layer. Refuse
 * redirects, compression and chunked responses rather than interpreting them
 * as package bytes. Caddy supplies Content-Length for these regular files. */
int pkg_download(const char *url, const char *output, unsigned long limit) {
    char host[256], path[512], request[1024], line[512], *slash, *colon, *end;
    unsigned port = 80, headers = 0;
    long parsed;
    int fd = -1, result = -1, n, seen = 0;
    unsigned long length = 0, total = 0;
    FILE *f = NULL;
#ifdef RIPPLE_MMU
    struct hostent *entry;
    struct sockaddr_in address;
#endif
    PkgByte buffer[2048];
    if (strncmp(url, "http://", 7) || strlen(url) >= 500)
        return pkg_fail("repository requires an http:// static URL", url);
    slash = strchr(url + 7, '/');
    n = slash ? (int)(slash - (url + 7)) : (int)strlen(url + 7);
    if (!n || n >= 256)
        return pkg_fail("invalid repository host", url);
    memcpy(host, url + 7, n);
    host[n] = 0;
    strcpy(path, slash ? slash : "/");
    if (strpbrk(host, "@?# \r\n") || strpbrk(path, "?# \r\n"))
        return pkg_fail("invalid repository URL", url);
    colon = strchr(host, ':');
    if (colon) {
        *colon++ = 0;
        parsed = strtol(colon, &end, 10);
        if (!*colon || *end || parsed < 1 || parsed > 65535)
            return pkg_fail("invalid repository port", url);
        port = (unsigned)parsed;
    }
#ifndef RIPPLE_MMU
    fd = host_connect(host, port);
    if (fd < 0) {
        pkg_fail("cannot connect to repository", host);
        goto done;
    }
#else
    entry = gethostbyname(host);
    if (!entry || entry->h_addrtype != AF_INET || entry->h_length != sizeof(address.sin_addr))
        return pkg_fail("cannot resolve repository", host);
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    /* macOS's legacy resolver may return an unaligned pointer vector. */
    {
        char *packed;
        memcpy(&packed, entry->h_addr_list, sizeof(packed));
        memcpy(&address.sin_addr, packed, sizeof(address.sin_addr));
    }
    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0 || connect(fd, (struct sockaddr *)&address, sizeof(address))) {
        pkg_fail("cannot connect to repository", host);
        goto done;
    }
#endif
    snprintf(
        request, sizeof(request),
        "GET %s HTTP/1.0\r\nHost: %s:%u\r\nAccept-Encoding: identity\r\nConnection: close\r\n\r\n",
        path, host, port);
    {
        size_t sent = 0, size = strlen(request);
        while (sent < size) {
            ssize_t count;
            if (ready(fd, 1))
                goto done;
            count = send(fd, request + sent, size - sent, 0);
            if (count <= 0) {
                pkg_fail("HTTP request failed", host);
                goto done;
            }
            sent += (size_t)count;
        }
    }
    if (http_line(fd, line) || strncmp(line, "HTTP/1.", 7) || strlen(line) < 12 ||
        strncmp(line + 8, " 200", 4)) {
        pkg_fail("repository HTTP status rejected", url);
        goto done;
    }
    for (;;) {
        if (++headers > 64 || http_line(fd, line))
            goto done;
        if (!*line)
            break;
        if (!strncasecmp(line, "Content-Length:", 15)) {
            char *v = line + 15;
            while (*v == ' ')
                v++;
            errno = 0;
            length = strtoul(v, &end, 10);
            if (seen++ || errno || *v < '0' || *v > '9' || *end || length > limit) {
                pkg_fail("invalid/oversized HTTP body", url);
                goto done;
            }
        } else if (!strncasecmp(line, "Transfer-Encoding:", 18) ||
                   !strncasecmp(line, "Content-Encoding:", 17)) {
            pkg_fail("unsupported HTTP encoding", url);
            goto done;
        }
    }
    if (!seen) {
        pkg_fail("HTTP response missing length", url);
        goto done;
    }
    f = fopen(output, "wb");
    if (!f) {
        pkg_fail("cannot create download", output);
        goto done;
    }
    while (total < length) {
        unsigned want = length - total > 2048 ? 2048 : (unsigned)(length - total);
        n = receive(fd, buffer, want);
        if (n < 0)
            goto done;
        if (fwrite(buffer, 1, (size_t)n, f) != (size_t)n) {
            pkg_fail("download write failed", output);
            goto done;
        }
        total += (unsigned long)n;
    }
    if (fclose(f)) {
        f = NULL;
        pkg_fail("download close failed", output);
        goto done;
    }
    f = NULL;
    result = 0;
done:
    if (fd >= 0)
        close(fd);
    if (f)
        fclose(f);
    if (result)
        unlink(output);
    return result;
}
static Repository *repository(const char *root) {
    Repository *r = calloc(1, sizeof(*r));
    char config[512], base[512], text[512];
    FILE *f;
    if (!r)
        return NULL;
    r->lock = -1;
    if (pkg_join(config, root, "etc/pkg/repo.conf") || pkg_join(r->key, root, "etc/pkg/repo.pub") ||
        pkg_join(r->cache, root, "var/cache/pkg") || pkg_mkdirs(r->cache))
        goto failed;
    if (pkg_join(base, r->cache, "lock"))
        goto failed;
    r->lock = open(base, O_RDWR | O_CREAT, 0600);
    if (r->lock < 0 || flock(r->lock, LOCK_EX | LOCK_NB)) {
        pkg_fail("repository cache is locked", base);
        goto failed;
    }
    f = fopen(config, "r");
    if (!f) {
        pkg_fail("missing repo.conf containing url=http://...", config);
        goto failed;
    }
    if (!fgets(text, sizeof(text), f) || strncmp(text, "url=", 4) || !strchr(text, '\n') ||
        fgetc(f) != EOF || strlen(text + 4) > 255) {
        fclose(f);
        pkg_fail("invalid repository configuration", config);
        goto failed;
    }
    fclose(f);
    text[strlen(text) - 1] = 0;
    strcpy(r->url, text + 4);
    while (*r->url && r->url[strlen(r->url) - 1] == '/')
        r->url[strlen(r->url) - 1] = 0;
    if (strncmp(r->url, "http://", 7) || pkg_join(base, r->cache, "current") ||
        pkg_join(r->index, base, "index.v1") || pkg_join(r->signature, base, "index.sig")) {
        pkg_fail("invalid repository URL", r->url);
        goto failed;
    }
    return r;
failed:
    if (r->lock >= 0)
        close(r->lock);
    free(r);
    return NULL;
}
static int entries(const char *index, Entry *all, unsigned *count) {
    char text[768];
    FILE *f = fopen(index, "r");
    unsigned n = 0, i;
    int failed = 1;
    if (!f)
        return pkg_fail("missing cached index; run pkg update", index);
    if (!fgets(text, sizeof(text), f) || strcmp(text, "RIPINDEX1\n"))
        goto done;
    while (fgets(text, sizeof(text), f)) {
        char *fields[7], *at = text, *end;
        unsigned j;
        Entry *e;
        if (n == PKG_ENTRIES)
            goto done;
        e = &all[n];
        if (!*text || text[strlen(text) - 1] != '\n')
            goto done;
        text[strlen(text) - 1] = 0;
        for (j = 0; j < 7; j++) {
            fields[j] = at;
            if (j < 6) {
                at = strchr(at, '\t');
                if (!at)
                    goto done;
                *at++ = 0;
            }
        }
        if (strchr(fields[6], '\t') || !pkg_token(fields[0], 24) || !pkg_token(fields[1], 31) ||
            !pkg_token(fields[2], 63) || !pkg_digest_ok(fields[4]) || strlen(fields[5]) != 36 ||
            strncmp(fields[5], "objects/", 8) || strncmp(fields[5] + 8, fields[4], 24) ||
            strcmp(fields[5] + 32, ".rpk") || strlen(fields[6]) > 255)
            goto done;
        errno = 0;
        e->size = strtoul(fields[3], &end, 10);
        if (errno || *fields[3] < '0' || *fields[3] > '9' || *end || e->size > PKG_LIMIT)
            goto done;
        strcpy(e->name, fields[0]);
        strcpy(e->version, fields[1]);
        strcpy(e->target, fields[2]);
        strcpy(e->hash, fields[4]);
        strcpy(e->path, fields[5]);
        strcpy(e->depends, fields[6]);
        for (i = 0; i < n; i++)
            if (!strcmp(all[i].name, e->name) && !strcmp(all[i].target, e->target))
                goto done;
        n++;
    }
    if (ferror(f))
        goto done;
    *count = n;
    failed = 0;
done:
    fclose(f);
    return failed ? pkg_fail("invalid repository index", index) : 0;
}
int pkg_update(const char *root) {
    Repository *r = repository(root);
    Entry *all = calloc(PKG_ENTRIES, sizeof(*all));
    unsigned count;
    char stage[512], index[512], sig[512], current[512], previous[512], url[512];
    int failed = 1;
    if (!r || !all)
        goto done;
    if (pkg_join(stage, r->cache, "staging") || pkg_remove_tree(stage) || pkg_mkdirs(stage) ||
        pkg_join(index, stage, "index.v1") || pkg_join(sig, stage, "index.sig") ||
        pkg_join(url, r->url, "index.v1"))
        goto done;
    if (pkg_download(url, index, PKG_INDEX_LIMIT) || pkg_join(url, r->url, "index.sig") ||
        pkg_download(url, sig, 256UL) || pkg_verify(index, sig, r->key) ||
        entries(index, all, &count))
        goto done;
    if (pkg_join(current, r->cache, "current") || pkg_join(previous, r->cache, "previous") ||
        pkg_remove_tree(previous))
        goto done;
    if (access(current, F_OK) == 0 && rename(current, previous)) {
        pkg_fail("cannot retain cached index", current);
        goto done;
    }
    if (rename(stage, current)) {
        rename(previous, current);
        pkg_fail("cannot publish cached index", current);
        goto done;
    }
    printf("Verified repository index: %u packages\n", count);
    failed = 0;
done:
    free(all);
    if (r) {
        if (r->lock >= 0)
            close(r->lock);
        free(r);
    }
    return failed ? -1 : 0;
}
static int add_entry(const char *root, const char *target, Repository *r, Entry *all,
                     unsigned count, unsigned at, unsigned *state, unsigned depth) {
    Entry *e = &all[at];
    char dependencies[256], *dep, *next, *version, db[512], name[40], url[512], download[512],
        hash[65];
    Package *p = calloc(1, sizeof(*p));
    unsigned long size;
    unsigned i;
    int failed = 1;
    if (!p)
        return pkg_fail("out of memory", "");
    if (depth > 16) {
        free(p);
        return pkg_fail("dependency nesting limit", e->name);
    }
    if (state[at] == 2) {
        free(p);
        return 0;
    }
    if (state[at] == 1) {
        free(p);
        return pkg_fail("dependency cycle", e->name);
    }
    state[at] = 1;
    strcpy(dependencies, e->depends);
    dep = dependencies;
    while (*dep) {
        next = strchr(dep, ',');
        if (next)
            *next++ = 0;
        version = strchr(dep, '@');
        if (!version) {
            pkg_fail("invalid dependency in index", dep);
            goto done;
        }
        *version++ = 0;
        for (i = 0; i < count; i++)
            if (!strcmp(all[i].name, dep) &&
                (!strcmp(version, "*") || !strcmp(all[i].version, version)) &&
                (!strcmp(all[i].target, target) || !strcmp(all[i].target, "any")))
                break;
        if (i == count) {
            pkg_fail("dependency unavailable in repository", dep);
            goto done;
        }
        if (add_entry(root, target, r, all, count, i, state, depth + 1))
            goto done;
        if (!next)
            break;
        dep = next;
        if (!*dep) {
            pkg_fail("empty index dependency", "");
            goto done;
        }
    }
    snprintf(name, sizeof(name), "%s.db", e->name);
    if (pkg_join(db, root, "var/lib/pkg/db") || pkg_join(download, db, name))
        goto done;
    if (access(download, F_OK) == 0) {
        if (pkg_db_read(download, p))
            goto done;
        if (!strcmp(p->hash, e->hash)) {
            state[at] = 2;
            failed = 0;
            goto done;
        }
    }
    if (pkg_join(url, r->url, e->path) || pkg_join(download, r->cache, "download.rpk"))
        goto done;
    printf("Downloading %s %s (%lu bytes)\n", e->name, e->version, e->size);
    fflush(stdout);
    if (pkg_download(url, download, e->size) || pkg_hash_file(download, hash, &size) ||
        size != e->size || strcmp(hash, e->hash)) {
        pkg_fail("repository object digest/length mismatch", e->name);
        goto done;
    }
    if (pkg_inspect(download, p, NULL) || strcmp(p->name, e->name) ||
        strcmp(p->version, e->version) || strcmp(p->target, e->target) ||
        strcmp(p->depends, e->depends)) {
        pkg_fail("package does not match signed index", e->name);
        goto done;
    }
    if (pkg_install(root, target, download))
        goto done;
    unlink(download);
    state[at] = 2;
    failed = 0;
done:
    free(p);
    return failed ? -1 : 0;
}
int pkg_add(const char *root, const char *target, const char *name) {
    Repository *r = repository(root);
    Entry *all = calloc(PKG_ENTRIES, sizeof(*all));
    unsigned *state = calloc(PKG_ENTRIES, sizeof(*state)), count, i;
    int failed = 1;
    if (!r || !all || !state)
        goto done;
    if (pkg_verify(r->index, r->signature, r->key) || entries(r->index, all, &count))
        goto done;
    for (i = 0; i < count; i++)
        if (!strcmp(all[i].name, name) && !strcmp(all[i].target, target))
            break;
    if (i == count)
        for (i = 0; i < count; i++)
            if (!strcmp(all[i].name, name) && !strcmp(all[i].target, "any"))
                break;
    if (i == count) {
        pkg_fail("package unavailable for target", name);
        goto done;
    }
    failed = add_entry(root, target, r, all, count, i, state, 0) != 0;
done:
    free(state);
    free(all);
    if (r) {
        if (r->lock >= 0)
            close(r->lock);
        free(r);
    }
    return failed ? -1 : 0;
}

int pkg_search(const char *root, const char *target, const char *query) {
    Repository *r = repository(root);
    Entry *all = calloc(PKG_ENTRIES, sizeof(*all));
    unsigned count, i;
    int failed = 1;
    if (!r || !all)
        goto done;
    if (pkg_verify(r->index, r->signature, r->key) || entries(r->index, all, &count))
        goto done;
    for (i = 0; i < count; i++)
        if ((!strcmp(all[i].target, target) || !strcmp(all[i].target, "any")) &&
            strstr(all[i].name, query))
            printf("%s %s [%s]\n", all[i].name, all[i].version, all[i].target);
    failed = 0;
done:
    free(all);
    if (r) {
        if (r->lock >= 0)
            close(r->lock);
        free(r);
    }
    return failed ? -1 : 0;
}
