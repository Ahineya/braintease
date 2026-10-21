/* Deterministic ar creation. Recreate from the complete member list, never
 * append in place; failed input/output leaves the prior library untouched. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
static char *pending;
static unsigned long archive_size = 8;

static void cleanup(void)
{
    if (pending)
        unlink(pending);
}

static void die(const char *message)
{
    fprintf(stderr, "rcc-x64-ar: %s\n", message);
    exit(1);
}

static void write_bytes(FILE *f, const void *p, size_t n)
{
    if (fwrite(p, 1, n, f) != n)
        die("cannot write archive");
}

static const char *basename(const char *path)
{
    const char *p = strrchr(path, '/');
    return p ? p + 1 : path;
}

static void field(char *header, unsigned at, unsigned width, unsigned long value)
{
    char text[32];
    size_t n;
    sprintf(text, "%lu", value);
    n = strlen(text);
    if (n > width)
        die("archive field overflow");
    memcpy(header + at, text, n);
}

static void member(FILE *out, const char *path)
{
    const char *name = basename(path);
    size_t n = strlen(name), i;
    int extended = n > 15 || strchr(name, ' ') != NULL;
    FILE *in = fopen(path, "rb");
    long length = 0;
    unsigned long size, remaining;
    unsigned char buffer[512];
    char header[60];
    if (!n || n > 1024 || !strcmp(name, "#1") || !strcmp(name, "__.SYMDEF") ||
        !strncmp(name, "__.SYMDEF ", 10) || !strcmp(name, "__.SYMDEF_64") ||
        !strcmp(name, "__.SYMDEF_64 SORTED"))
        die("invalid/reserved member name");
    for (i = 0; i < n; i++)
        if ((unsigned char)name[i] < 32 || (unsigned char)name[i] > 126)
            die("member names must be printable ASCII");
    if (!in || fseek(in, 0, SEEK_END) || (length = ftell(in)) < 64 ||
        (unsigned long)length > 16777216UL || fseek(in, 0, SEEK_SET))
        die("cannot read bounded ELF object");
    if (fread(buffer, 1, 8, in) != 8 || memcmp(buffer, "\177ELF\2\1\1", 7) ||
        fseek(in, 0, SEEK_SET))
        die("expected ELF64 little-endian object");
    size = (unsigned long)length + (extended ? n : 0);
    if (archive_size > 134217728UL - 60 || size + (size & 1) > 134217728UL - archive_size - 60)
        die("archive exceeds 128 MiB capacity");
    archive_size += 60 + size + (size & 1);
    memset(header, ' ', sizeof header);
    if (extended) {
        memcpy(header, "#1/", 3);
        field(header, 3, 13, n);
    } else {
        memcpy(header, name, n);
        header[n] = '/';
    }
    field(header, 16, 12, 0);
    field(header, 28, 6, 0);
    field(header, 34, 6, 0);
    memcpy(header + 40, "100644", 6);
    field(header, 48, 10, size);
    header[58] = '`';
    header[59] = '\n';
    write_bytes(out, header, sizeof header);
    if (extended)
        write_bytes(out, name, n);
    remaining = (unsigned long)length;
    while (remaining) {
        size_t count = remaining > sizeof buffer ? sizeof buffer : (size_t)remaining;
        if (fread(buffer, 1, count, in) != count)
            die("cannot read object payload");
        for (i = 0; i < count; i++)
            if (buffer[i] > 255)
                die("object contains non-octet cells");
        write_bytes(out, buffer, count);
        remaining -= count;
    }
    if (fgetc(in) != EOF || ferror(in) || fclose(in))
        die("object changed during archive creation");
    if (size & 1)
        write_bytes(out, "\n", 1);
}

int main(int argc, char **argv)
{
    const char *output = NULL;
    FILE *out;
    int i, j, fd;
    unsigned count = 0;
    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-o") && !output && i + 1 < argc)
            output = argv[++i];
        else if (!strcmp(argv[i], "--help")) {
            puts("Usage: rcc-x64-ar -o LIBRARY.a INPUT.o...");
            puts("Recreates a deterministic ar library from the complete member list.");
            return 0;
        } else if (argv[i][0] == '-')
            die("invalid option (use --help)");
        else if (++count > 4096)
            die("too many archive members");
    }
    if (!output)
        die("an output library is required");
    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-o")) {
            i++;
            continue;
        }
        if (!strcmp(output, argv[i]))
            die("output must not overwrite input");
        for (j = 1; j < i; j++) {
            if (!strcmp(argv[j], "-o")) {
                j++;
                continue;
            }
            if (!strcmp(basename(argv[i]), basename(argv[j])))
                die("duplicate archive member basename");
        }
    }
    pending = malloc(strlen(output) + 40);
    if (!pending)
        die("out of memory");
    sprintf(pending, "%s.tmp.%u", output, (unsigned)getpid());
    fd = open(pending, O_CREAT | O_EXCL | O_WRONLY, 0600);
    if (fd < 0)
        die("cannot create temporary archive");
    atexit(cleanup);
    if (close(fd))
        die("cannot close temporary archive");
    out = fopen(pending, "wb");
    if (!out)
        die("cannot open temporary archive");
    write_bytes(out, "!<arch>\n", 8);
    for (i = 1; i < argc; i++)
        if (!strcmp(argv[i], "-o"))
            i++;
        else
            member(out, argv[i]);
    if (fflush(out) || ferror(out) || fclose(out))
        die("cannot finish archive");
    if (rename(pending, output))
        die("cannot publish archive");
    pending = NULL;
    return 0;
}
