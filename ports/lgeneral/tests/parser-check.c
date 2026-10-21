/* Parser regression harness; link with the adapted upstream parser.c/list.c. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "parser.h"

static long failures;
#define CHECK(c, label) do { if (!(c)) { \
    fprintf(stderr, "FAIL parser: %s\n", label); failures++; \
} } while (0)

static unsigned long node_count(PData *pd)
{
    unsigned long count = 1;
    ListIterator it;
    PData *child;
    if (!pd || !pd->entries) return pd ? 1 : 0;
    it = list_iterator(pd->entries);
    while ((child = list_iterator_next(&it)) != 0) count += node_count(child);
    return count;
}

int main(int argc, char **argv)
{
    char path[512];
    FILE *file;
    PData *pd;
    char *value;
    long i;
    if (argc != 3) {
        fprintf(stderr, "usage: parser-check DATA-DIR WRITABLE-TEST-DIR\n");
        return 2;
    }
    snprintf(path, sizeof(path), "%s/units/kukgen.udb", argv[1]);
    fprintf(stderr,"parser: loading unit database\n");
    pd = parser_read_file("units", path);
    fprintf(stderr,"parser: unit database loaded\n");
    CHECK(pd != 0, "151 KiB K.u.k. unit database loads without file-size truncation");
    if (pd) {
        unsigned long count = node_count(pd);
        CHECK(count > 1000, "complete unit database, including late sections");
        printf("PARSER unit-db nodes=%lu\n", count);
        parser_free(&pd);
    }

    snprintf(path, sizeof(path), "%s/parser-compact", argv[2]);
    file = fopen(path, "wb");
    CHECK(file != 0, "create compact fixture");
    if (!file) return 1;
    fputs("@\n<outer\n", file);
    for (i = 0; i < 12000; ++i) fputs("x=value\n", file);
    /* A long token exercises the heap path; old 8-bit markers must remain
     * numeric logical octets on C16, with UTF-8 payload bytes unchanged. */
    fputs("last", file); fputc(0xbb, file);
    for (i = 0; i < 1200; ++i) fputc('z', file);
    fputc(0xb0, file); fputs("utf8:", file);
    fputc(0xd0, file); fputc(0x96, file);
    fputs("\n>\n", file); fclose(file);
    pd = parser_read_file("fixture", path);
    CHECK(pd != 0, "compact resource beyond 64 KiB");
    if (pd) {
        CHECK(parser_get_value(pd, "outer/last", &value, 0) && strlen(value) == 1200,
              "long compact value survives token allocation");
        CHECK(parser_get_value(pd, "outer/last", &value, 1) &&
              strlen(value) == 7 && (unsigned char)value[5] == 0xd0 &&
              (unsigned char)value[6] == 0x96, "legacy delimiters preserve UTF-8 octets");
        parser_free(&pd);
    }

    file = fopen(path, "wb");
    CHECK(file != 0, "create oversize fixture");
    if (!file) return 1;
    fputs("@\n", file);
    for (i = 0; i < 262144L; ++i) fputc('a', file);
    fclose(file);
    pd = parser_read_file("oversize", path);
    CHECK(pd == 0, "oversize resource rejected instead of partially loaded");
    if (pd) parser_free(&pd);

    file = fopen(path, "wb");
    CHECK(file != 0, "create malformed fixture");
    if (!file) return 1;
    fputs("@\n<outer\nmissing-assignment\n>\n", file); fclose(file);
    pd = parser_read_file("malformed", path);
    CHECK(pd == 0, "nested compact parse error reaches caller");
    if (pd) parser_free(&pd);
    remove(path);
    printf("PARSER %s\n", failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
