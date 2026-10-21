/* SPDX-License-Identifier: MIT */
#include "csv.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned long steps;
static void seed(struct csv_document *d) {
    char error[180];
    const char *s = "name,value\nprevious,7\n";
    assert(!csv_parse(d, s, strlen(s), ',', 1, error, sizeof(error)));
}
static unsigned long drive(struct csv_job *j, struct csv_document *d, size_t budget) {
    char *data = d->data;
    uint32_t *order = d->order, rows = d->row_count;
    unsigned progress = csv_job_progress(j);
    unsigned long count = 0;
    int status;
    assert(j && csv_job_step(j, 0) == CSV_JOB_RUNNING);
    do {
        status = csv_job_step(j, budget);
        count++;
        assert(csv_job_progress(j) >= progress && csv_job_progress(j) <= 1000);
        progress = csv_job_progress(j);
        if (status == CSV_JOB_RUNNING) {
            assert(d->data == data && d->order == order && d->row_count == rows);
        }
        assert(count < 10000000UL);
    } while (status == CSV_JOB_RUNNING);
    if (status != CSV_JOB_DONE)
        fprintf(stderr, "Job failed: %s\n", csv_job_error(j));
    assert(status == CSV_JOB_DONE && csv_job_progress(j) == 1000);
    assert(csv_job_step(j, budget) == CSV_JOB_DONE);
    csv_job_cancel(j);
    assert(csv_job_step(j, budget) == CSV_JOB_DONE);
    csv_job_free(j);
    steps += count;
    return count;
}
static void cancellation(struct csv_document *d, const char *text, const char *phase) {
    char error[180];
    char *data = d->data;
    uint32_t *order = d->order, rows = d->row_count;
    struct csv_job *j = csv_parse_begin(d, text, strlen(text), 0, 1, error, sizeof(error));
    int status = CSV_JOB_RUNNING;
    assert(j);
    while (status == CSV_JOB_RUNNING && strcmp(csv_job_phase(j), phase))
        status = csv_job_step(j, 1);
    assert(status == CSV_JOB_RUNNING);
    csv_job_cancel(j);
    assert(csv_job_step(j, 10000) == CSV_JOB_CANCELLED);
    assert(!strcmp(csv_job_phase(j), "Cancelled"));
    csv_job_free(j);
    assert(d->data == data && d->order == order && d->row_count == rows);
    assert(!strcmp(csv_cell(d, 0, 0), "previous"));
}
static void error_keeps_document(struct csv_document *d, const char *text, uint32_t bytes) {
    char error[180];
    char *data = d->data;
    uint32_t *order = d->order;
    struct csv_job *j = csv_parse_begin(d, text, bytes, 0, 1, error, sizeof(error));
    int status;
    assert(j);
    do {
        status = csv_job_step(j, 1);
    } while (status == CSV_JOB_RUNNING);
    assert(status == CSV_JOB_ERROR && *csv_job_error(j));
    csv_job_free(j);
    assert(d->data == data && d->order == order && !strcmp(csv_cell(d, 0, 0), "previous"));
}
static void sort_cancellation(struct csv_document *d, const char *phase) {
    char error[180];
    uint32_t *order = d->order, i;
    uint32_t *snapshot = malloc(d->row_count * sizeof(*snapshot));
    struct csv_job *j = csv_sort_begin(d, 1, 0, error, sizeof(error));
    int status = CSV_JOB_RUNNING;
    assert(snapshot && j);
    memcpy(snapshot, d->order, d->row_count * sizeof(*snapshot));
    while (status == CSV_JOB_RUNNING && strcmp(csv_job_phase(j), phase))
        status = csv_job_step(j, 1);
    assert(status == CSV_JOB_RUNNING);
    csv_job_cancel(j);
    assert(csv_job_step(j, 100) == CSV_JOB_CANCELLED);
    assert(d->order == order && d->sort_column == -1);
    for (i = 0; i < d->row_count; i++)
        assert(d->order[i] == snapshot[i]);
    csv_job_free(j);
    free(snapshot);
}
int main(int argc, char **argv) {
    struct csv_document d;
    char error[180];
    const char *small = "name,value,note\r\n\"A, B\",10,\"two\nlines\"\r\n"
        "Київ,2,\"say \"\"hi\"\"\"\r\nD,,\r\nE,2,end";
    const char *phases[] = {"Copying", "Validating UTF-8", "Detecting delimiter",
        "Indexing records", "Decoding fields", "Preparing rows"};
    unsigned i;
    uint32_t row, at;
    memset(&d, 0, sizeof(d));
    if (argc > 2 && !strcmp(argv[1], "--sort")) {
        assert(!csv_load(&d, argv[2], ',', 0, error, sizeof(error)));
        drive(csv_sort_begin(&d, 0, argc > 3, error, sizeof(error)), &d, 17);
        for (row = 0; row < d.row_count; row++)
            puts(csv_cell(&d, row, 1));
        csv_free(&d);
        return 0;
    }
    {
        static const char *valid[] = {"2", "+.2e1", " 2\t", "-0.00", "0e999999"};
        static const double expected[] = {2.0, 2.0, 2.0, 0.0, 0.0};
        static const char *invalid[] = {"", " \t", "nan", "inf", "1e999999", "1e-999999",
                                       "+", ".", "1x", "2\n", "2e+"};
        csv_number *number = csv_number_create();
        int result;
        assert(number);
        for (i = 0; i < sizeof(valid) / sizeof(*valid); i++) {
            csv_number_reset(number, valid[i]);
            assert(!csv_number_step(number, 0));
            do { result = csv_number_step(number, 1); } while (!result);
            assert(result == 1 && !csv_number_empty(number));
            assert(csv_number_value(number) == expected[i]);
            assert(csv_number_step(number, 100) == 1);
        }
        for (i = 0; i < sizeof(invalid) / sizeof(*invalid); i++) {
            csv_number_reset(number, invalid[i]);
            do { result = csv_number_step(number, 1); } while (!result);
            assert(result == -1 && csv_number_empty(number) == (i < 2));
        }
        csv_number_reset(number, "1e999999");
        assert(!csv_number_step(number, 2));
        csv_number_reset(number, "2");
        assert(csv_number_step(number, 100) == 1 && csv_number_value(number) == 2.0);
        csv_number_delete(number);
    }
    seed(&d);
    for (i = 0; i < sizeof(phases) / sizeof(*phases); i++)
        cancellation(&d, small, phases[i]);
    error_keeps_document(&d, "name\n\"bad", 9);
    {
        const char nul[] = {'x', 0, 'y'}, overlong[] = {0xe0, 0x80, 0x80};
        const char surrogate[] = {0xed, 0xa0, 0x80}, missing[] = {0xf0, 0x90, 0x80};
        error_keeps_document(&d, nul, 3);
        error_keeps_document(&d, overlong, 3);
        error_keeps_document(&d, surrogate, 3);
        error_keeps_document(&d, missing, 3);
    }
    drive(csv_parse_begin(&d, small, strlen(small), 0, 1, error, sizeof(error)), &d, 1);
    assert(d.row_count == 4 && d.columns == 3);
    assert(!strcmp(csv_cell(&d, 1, 0), "Київ"));
    assert(!strcmp(csv_cell(&d, 1, 2), "say \"hi\""));
    sort_cancellation(&d, "Reading sort keys");
    sort_cancellation(&d, "Sorting");
    drive(csv_sort_begin(&d, 1, 0, error, sizeof(error)), &d, 1);
    assert(d.order[0] == 2 && d.order[1] == 4 && d.order[2] == 1 && d.order[3] == 3);
    drive(csv_sort_begin(&d, 1, 1, error, sizeof(error)), &d, 1);
    assert(d.order[0] == 1 && d.order[1] == 2 && d.order[2] == 4 && d.order[3] == 3);
    csv_header(&d, 0);
    assert(d.row_count == 5 && d.order[4] == 4);
    csv_header(&d, 1);
    assert(d.row_count == 4 && d.order[3] == 4);
    {
        /* Numeric escapes in C16 string literals are Unicode codepoints; use
         * explicit bytes for a UTF-8 BOM fixture shared with the host test. */
        const char s[] = {239,187,191,'n','a','m','e','\t','v','a','l','u','e','\n',
                          'x','\t','2','\n',0};
        drive(csv_parse_begin(&d, s, strlen(s), 0, 1, error, sizeof(error)), &d, 1);
        assert(d.delimiter == '\t' && !strcmp(csv_heading(&d, 0), "name"));
    }
    /* A worst-case shared string prefix cannot hide an unbounded strcmp in a
     * single merge step. Nor can a huge numeric cell hide an unbounded strtod. */
    {
        uint32_t length = 131072UL;
        char *text = malloc(length * 2UL + 16UL);
        struct csv_job *j;
        unsigned long count;
        assert(text);
        memset(text, 'a', length);
        text[length] = 'Z';
        text[length + 1] = '\n';
        memset(text + length + 2, 'a', length);
        text[length * 2 + 2] = 'A';
        text[length * 2 + 3] = '\n';
        drive(csv_parse_begin(&d, text, length * 2 + 4, ',', 0, error, sizeof(error)), &d, 4096);
        j = csv_sort_begin(&d, 0, 0, error, sizeof(error));
        while (!strcmp(csv_job_phase(j), "Reading sort keys"))
            assert(csv_job_step(j, 1) == CSV_JOB_RUNNING);
        for (i = 0; i < 100; i++) {
            assert(csv_job_step(j, 1) == CSV_JOB_RUNNING);
            assert(d.order[0] == 0);
        }
        csv_job_cancel(j);
        csv_job_free(j);
        count = drive(csv_sort_begin(&d, 0, 0, error, sizeof(error)), &d, 4096);
        assert(count > 90 && d.order[0] == 1);
        memset(text, '0', length);
        memcpy(text + length, "2\n1\n", 4);
        drive(csv_parse_begin(&d, text, length + 4, ',', 0, error, sizeof(error)), &d, 4096);
        count = drive(csv_sort_begin(&d, 0, 0, error, sizeof(error)), &d, 4096);
        assert(count > 30 && d.order[0] == 1);
        free(text);
    }
    /* Stable numeric sorting, scientific notation and whitespace. Hex parsing
     * follows libc availability and is covered by the host differential test. */
    {
        const char *s = "id,n\na,8e0\nb,+2.0e0\nc,.2e1\nd,  \ne,-.1e2\nf,1.5\n";
        assert(!csv_parse(&d, s, strlen(s), ',', 1, error, sizeof(error)));
        drive(csv_sort_begin(&d, 1, 0, error, sizeof(error)), &d, 2);
        assert(d.order[0] == 5 && d.order[1] == 6 && d.order[2] == 2 &&
               d.order[3] == 3 && d.order[4] == 1 && d.order[5] == 4);
    }
    /* Actual reads are chunked too. Optional writable path permits running this
     * unchanged in a disposable guest, without host filesystem assumptions. */
    if (argc > 1) {
        FILE *file = fopen(argv[1], "wb");
        char *data;
        struct csv_job *j;
        assert(file);
        assert(fwrite(small, 1, strlen(small), file) == strlen(small));
        assert(!fclose(file));
        data = d.data;
        j = csv_load_begin(&d, argv[1], 0, 1, error, sizeof(error));
        assert(j && !strcmp(csv_job_phase(j), "Reading"));
        assert(csv_job_step(j, 1) == CSV_JOB_RUNNING);
        csv_job_cancel(j);
        csv_job_free(j);
        assert(d.data == data);
        drive(csv_load_begin(&d, argv[1], 0, 1, error, sizeof(error)), &d, 1);
        assert(!strcmp(csv_cell(&d, 1, 0), "Київ"));
        remove(argv[1]);
    }
    {
        char *text = malloc(2200000UL);
        assert(text);
        memcpy(text, "id,value\n", 9);
        at = 9;
        for (row = 0; row < CSV_MAX_ROWS; row++)
            at += snprintf(text + at, 2200000UL - at, "%lu,%lu\n",
                (unsigned long)row, (unsigned long)(CSV_MAX_ROWS - row));
        assert(drive(csv_parse_begin(&d, text, at, ',', 1, error, sizeof(error)), &d, 4096) > 100);
        assert(d.row_count == CSV_MAX_ROWS);
        assert(drive(csv_sort_begin(&d, 1, 0, error, sizeof(error)), &d, 4096) > 100);
        assert(d.order[0] == CSV_MAX_ROWS && d.order[CSV_MAX_ROWS - 1] == 1);
        for (row = 1; row < CSV_MAX_ROWS; row++)
            assert(d.order[row - 1] == d.order[row] + 1);
        free(text);
    }
    csv_free(&d);
    printf("CSV_JOBS passed bounded phases, cancellation, transactional failures, UTF-8, "
           "long comparisons/numbers, stable sorts, header toggles and 100k rows (%lu steps)\n", steps);
    return 0;
}
