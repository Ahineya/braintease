/* ar cataloguing keeps only export names and file spans for unused members.
 * Extraction follows input order, rescanning each archive to a fixed point. */
#include "link.h"
#include <stdlib.h>
#include <string.h>

#define ARCHIVE_LIMIT 134217728UL

struct requirement {
    struct requirement *next;
    char *name;
    int needed, supplied;
};

struct member {
    char *name, *display;
    L32 offset, size;
    struct ld_vector exports;
    int included;
};
static struct requirement *requirements[4096];

static char *copy(const char *s)
{
    char *p = ld_memory(strlen(s) + 1);
    strcpy(p, s);
    return p;
}

static struct requirement *requirement(const char *name)
{
    unsigned h = 5381;
    const char *s = name;
    struct requirement *r;
    while (*s)
        h = (h * 33 + (unsigned char)*s++) & 4095;
    for (r = requirements[h]; r; r = r->next)
        if (!strcmp(r->name, name))
            return r;
    r = ld_memory(sizeof *r);
    r->name = copy(name);
    r->next = requirements[h];
    requirements[h] = r;
    return r;
}

static void selected(const char *name, int defined, void *context)
{
    struct requirement *r = requirement(name);
    (void)context;
    if (defined)
        r->supplied = 1;
    else
        r->needed = 1;
}

void ld_include(struct ld_object *o)
{
    struct ld_object **slot;
    if (ld_objects.count >= 256)
        ld_error("too many selected object inputs");
    ld_path = o->path;
    ld_object_symbols(o, selected, NULL);
    slot = ld_append(&ld_objects, sizeof *slot);
    *slot = o;
}

static void exported(const char *name, int defined, void *context)
{
    struct member *m = context;
    if (defined) {
        char **slot = ld_append(&m->exports, sizeof *slot);
        *slot = copy(name);
    }
}

static L32 decimal(const char *text, unsigned width)
{
    L32 n = 0;
    unsigned i = 0, count = 0;
    while (i < width && text[i] == ' ')
        i++;
    while (i < width && text[i] >= '0' && text[i] <= '9') {
        unsigned digit = text[i++] - '0';
        if (n > (ARCHIVE_LIMIT - digit) / 10)
            ld_error("archive quantity exceeds capacity");
        n = n * 10 + digit;
        count++;
    }
    while (i < width && text[i] == ' ')
        i++;
    if (!count || i != width)
        ld_error("invalid archive decimal field");
    return n;
}

static void read_at(FILE *f, L32 at, void *data, L32 size)
{
    L32 i;
    unsigned char *p = data;
    if (fseek(f, (long)at, SEEK_SET) || fread(data, 1, size, f) != size)
        ld_error("cannot read archive member");
    for (i = 0; i < size; i++)
        if (p[i] > 255)
            ld_error("archive contains non-octet cells");
}

static void archive(const char *path, FILE *f, L32 length)
{
    struct ld_vector members = {0};
    char *strings = NULL;
    L32 string_size = 0, at = 8;
    size_t i, j;
    int changed;
    while (at < length) {
        char header[61], name[1025], *label;
        L32 size, offset, skip = 0, n;
        struct member *m;
        struct ld_object *o;
        ld_path = path;
        if (length - at < 60)
            ld_error("truncated archive header");
        read_at(f, at, header, 60);
        if (header[58] != '`' || header[59] != '\n')
            ld_error("invalid archive header terminator");
        size = decimal(header + 48, 10);
        offset = at + 60;
        if (size > length - offset || (size & 1) > length - offset - size)
            ld_error("truncated archive payload/padding");
        if (size & 1) {
            char pad;
            read_at(f, offset + size, &pad, 1);
            if (pad != '\n')
                ld_error("invalid archive alignment padding");
        }
        at = offset + size + (size & 1);
        header[16] = 0;
        n = 16;
        while (n && header[n - 1] == ' ')
            header[--n] = 0;
        if (!strcmp(header, "//")) {
            if (strings || size > 1048576UL)
                ld_error("duplicate/oversized archive string table");
            strings = ld_memory(size + 1);
            string_size = size;
            read_at(f, offset, strings, size);
            continue;
        }
        if (!strcmp(header, "/") || !strcmp(header, "/SYM64/"))
            continue; /* Build our own C16-aware catalog; do not trust indexes. */
        if (!strncmp(header, "#1/", 3)) {
            skip = decimal(header + 3, n - 3);
            if (!skip || skip > 1024 || skip > size)
                ld_error("invalid BSD archive filename span");
            read_at(f, offset, name, skip);
            name[skip] = 0;
            n = skip;
            while (n && !name[n - 1])
                n--;
            name[n] = 0;
        } else if (header[0] == '/') {
            L32 start = decimal(header + 1, n - 1);
            if (!strings || start >= string_size || (start && strings[start - 1] != '\n'))
                ld_error("invalid GNU archive filename offset");
            for (n = 0; start + n + 1 < string_size && n < 1024; n++)
                if (strings[start + n] == '/' && strings[start + n + 1] == '\n')
                    break;
            if (n == 1024 || start + n + 1 >= string_size)
                ld_error("unterminated GNU archive filename");
            memcpy(name, strings + start, n);
            name[n] = 0;
        } else {
            if (n && header[n - 1] == '/')
                header[--n] = 0;
            memcpy(name, header, n + 1);
        }
        if (!n)
            ld_error("empty archive member name");
        for (j = 0; j < n; j++)
            if ((unsigned char)name[j] < 32 || (unsigned char)name[j] > 126)
                ld_error("invalid archive member name");
        if (!strcmp(name, "__.SYMDEF") || !strcmp(name, "__.SYMDEF SORTED") ||
            !strcmp(name, "__.SYMDEF_64") || !strcmp(name, "__.SYMDEF_64 SORTED"))
            continue;
        if (members.count >= 4096 || size - skip < 64 || size - skip > LD_LIMIT)
            ld_error("too many/invalid archive object members");
        m = ld_append(&members, sizeof *m);
        m->name = copy(name);
        label = ld_memory(strlen(path) + n + 3);
        sprintf(label, "%s(%s)", path, name);
        m->display = label;
        m->offset = offset + skip;
        m->size = size - skip;
        o = ld_read_object(path, m->offset, m->size);
        o->path = label;
        ld_path = label;
        ld_object_symbols(o, exported, m);
        ld_free_object(o);
    }
    do {
        struct member *ms = members.data;
        changed = 0;
        for (i = 0; i < members.count; i++) {
            char **names = ms[i].exports.data;
            if (ms[i].included)
                continue;
            for (j = 0; j < ms[i].exports.count; j++) {
                struct requirement *r = requirement(names[j]);
                if (r->needed && !r->supplied) {
                    struct ld_object *o = ld_read_object(path, ms[i].offset, ms[i].size);
                    o->path = ms[i].display;
                    ld_include(o);
                    ms[i].included = changed = 1;
                    break;
                }
            }
        }
    } while (changed);
    for (i = 0; i < members.count; i++) {
        struct member *m = &((struct member *)members.data)[i];
        char **names = m->exports.data;
        for (j = 0; j < m->exports.count; j++)
            free(names[j]);
        free(m->exports.data);
        free(m->name);
        if (!m->included)
            free(m->display);
    }
    free(members.data);
    free(strings);
}

void ld_input(const char *path)
{
    FILE *f = fopen(path, "rb");
    char magic[8];
    long length;
    ld_path = path;
    if (!f || fseek(f, 0, SEEK_END) || (length = ftell(f)) < 8 ||
        (unsigned long)length > ARCHIVE_LIMIT)
        ld_error("cannot read bounded object/archive");
    read_at(f, 0, magic, 8);
    if (!memcmp(magic, "!<arch>\n", 8))
        archive(path, f, (L32)length);
    else if (!memcmp(magic, "!<thin>\n", 8))
        ld_error("thin archives are not supported");
    else
        ld_object(path);
    if (fclose(f))
        ld_error("cannot close object/archive");
}
