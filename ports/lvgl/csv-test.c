/* SPDX-License-Identifier: MIT */
#include "csv.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
int main(int argc, char **argv) {
    struct csv_document d;
    char error[180], copy[512];
    uint32_t i;
    memset(&d, 0, sizeof(d));
    if (argc > 1) {
        if (csv_load(&d, argv[1], argc > 2 ? atoi(argv[2]) : 0, 0, error, sizeof(error))) {
            fprintf(stderr, "%s\n", error);
            return 1;
        }
        printf("%lu %u\n", (unsigned long)d.record_count, d.columns);
        for (i = 0; i < d.record_count; i++) {
            unsigned col;
            printf("%u", d.records[i].count);
            for (col = 0; col < d.records[i].count; col++) {
                const unsigned char *s = (const unsigned char *)csv_cell(&d, i, col);
                putchar(' ');
                for (; *s; s++)
                    printf("%02x", (unsigned)*s);
            }
            putchar('\n');
        }
        csv_free(&d);
        return 0;
    }
    {
        const char *s = "name,value,note\r\n\"A, B\",10,\"two\nlines\"\r\nC,2,\"say "
                        "\"\"hi\"\"\"\r\nD,,\r\nE,2,end";
        assert(!csv_parse(&d, s, strlen(s), 0, 1, error, sizeof(error)));
    }
    assert(d.row_count == 4 && d.columns == 3 && d.delimiter == ',');
    assert(!strcmp(csv_cell(&d, 0, 0), "A, B") && !strcmp(csv_cell(&d, 0, 2), "two\nlines"));
    assert(!strcmp(csv_cell(&d, 1, 2), "say \"hi\""));
    assert(!csv_sort(&d, 1, 0));
    assert(!strcmp(csv_cell(&d, 0, 0), "C") && !strcmp(csv_cell(&d, 1, 0), "E") &&
           !strcmp(csv_cell(&d, 2, 0), "A, B") && !strcmp(csv_cell(&d, 3, 0), "D"));
    assert(!csv_sort(&d, 1, 1));
    assert(!strcmp(csv_cell(&d, 0, 0), "A, B") && !strcmp(csv_cell(&d, 1, 0), "C") &&
           !strcmp(csv_cell(&d, 2, 0), "E") && !strcmp(csv_cell(&d, 3, 0), "D"));
    csv_original_order(&d);
    assert(!csv_copy_row(&d, 0, copy, sizeof(copy)));
    assert(!strcmp(copy, "A, B\t10\t\"two\nlines\""));
    assert(csv_copy_row(&d, 0, copy, 4) < 0);
    for (i = 0; i < 4; i++) {
        const char *bad[] = {"x,\"unfinished", "a\"b,c", "\"a\"x,b", "\"a\",\"b"};
        assert(csv_parse(&d, bad[i], strlen(bad[i]), ',', 1, error, sizeof(error)) < 0);
        assert(d.row_count == 4 && !strcmp(csv_cell(&d, 0, 0), "A, B"));
    }
    {
        const char bad[] = {0xc0, 0xaf};
        assert(csv_parse(&d, bad, 2, ',', 1, error, sizeof(error)) < 0);
    }
    {
        const char bad[] = {'a', 0, 'b'};
        assert(csv_parse(&d, bad, 3, ',', 1, error, sizeof(error)) < 0);
    }
    assert(!csv_parse(&d, "a,b,", 4, ',', 0, error, sizeof(error)));
    assert(d.row_count == 1 && d.columns == 3 && !*csv_cell(&d, 0, 2));
    assert(!csv_parse(&d, "\"\",\"\"\n\n", 7, ',', 0, error, sizeof(error)));
    assert(d.row_count == 2 && d.records[1].count == 1);
    assert(!csv_parse(&d, "", 0, 0, 1, error, sizeof(error)));
    assert(d.row_count == 0 && d.columns == 0);
    assert(!csv_parse(&d, "a\tb\n1\t2\n", 8, 0, 1, error, sizeof(error)));
    assert(d.delimiter == '\t' && d.row_count == 1 && !strcmp(csv_cell(&d, 0, 1), "2"));
    assert(!csv_parse(&d, "a;b\n1;2\n", 8, 0, 1, error, sizeof(error)));
    assert(d.delimiter == ';');
    {
        const char *s = "Ім'я,значення\nКиїв,12\nЛьвів,3\n";
        assert(!csv_parse(&d, s, strlen(s), 0, 1, error, sizeof(error)));
        assert(!strcmp(csv_cell(&d, 0, 0), "Київ"));
    }
    {
        char *s = malloc(CSV_MAX_COLUMNS * 2 + 4);
        unsigned n = 0;
        for (i = 0; i < CSV_MAX_COLUMNS; i++) {
            s[n++] = 'x';
            s[n++] = ',';
        }
        s[n++] = 'x';
        assert(csv_parse(&d, s, n, ',', 0, error, sizeof(error)) < 0);
        free(s);
    }
    {
        /* Each rejection must preserve the last successfully loaded document. */
        char *s = malloc(CSV_MAX_FIELDS + 2);
        assert(s);
        for (i = 0; i < CSV_MAX_FIELDS; i++)
            s[i] = (i + 1) % CSV_MAX_COLUMNS ? ',' : '\n';
        s[i++] = 'x';
        assert(csv_parse(&d, s, i, ',', 1, error, sizeof(error)) < 0);
        assert(strstr(error, "million"));
        memset(s, '\n', CSV_MAX_ROWS + 2);
        assert(csv_parse(&d, s, CSV_MAX_ROWS + 2, ',', 1, error, sizeof(error)) < 0);
        assert(strstr(error, "100,000"));
        assert(csv_parse(&d, s, CSV_MAX_BYTES + 1, ',', 1, error, sizeof(error)) < 0);
        assert(strstr(error, "4 MiB"));
        assert(!strcmp(csv_cell(&d, 0, 0), "Київ"));
        free(s);
    }
    csv_free(&d);
    puts("CSV_MODEL passed quoting, newlines, UTF-8, autodetection, bounds, transactional errors, "
         "numeric stable sort and clipboard");
    return 0;
}
