/* SPDX-License-Identifier: MIT */
#ifndef GRAPHITE_CSV_H
#define GRAPHITE_CSV_H
#include <stdint.h>
#include <stddef.h>
#define CSV_MAX_BYTES (4UL * 1024UL * 1024UL)
#define CSV_MAX_ROWS 100000UL
#define CSV_MAX_COLUMNS 64
#define CSV_MAX_FIELDS 1000000UL
struct csv_row {
    uint32_t first;
    unsigned count;
};
struct csv_document {
    char *data;
    uint32_t *fields, *order;
    struct csv_row *records;
    uint32_t record_count, field_count, row_count, bytes;
    unsigned columns, delimiter, header;
    int sort_column, descending;
};
struct csv_job;
enum csv_job_status {
    CSV_JOB_ERROR = -1,
    CSV_JOB_CANCELLED = -2,
    CSV_JOB_RUNNING = 0,
    CSV_JOB_DONE = 1
};
/* Jobs own their scratch storage. Keep doc alive and do not mutate it until the
 * job terminates. Load/sort commit only on DONE; cancellation/error preserve doc.
 * parse_begin borrows text until completion; copying also happens incrementally.
 * A step spends budget on bytes/rows/merge output, capped at 32768 units. String
 * comparison and numeric scanning are resumable even for a multi-megabyte cell.
 * Allocation, file metadata, a <= 4096-byte read, and bounded numeric conversion
 * may occur within a step. No document-sized copy or initialization is hidden.
 */
struct csv_job *csv_load_begin(struct csv_document *doc, const char *path, unsigned delimiter,
                               unsigned header, char *error, unsigned capacity);
struct csv_job *csv_parse_begin(struct csv_document *doc, const char *text, uint32_t bytes,
                                unsigned delimiter, unsigned header, char *error,
                                unsigned capacity);
struct csv_job *csv_sort_begin(struct csv_document *doc, unsigned column, int descending,
                               char *error, unsigned capacity);
int csv_job_step(struct csv_job *job, size_t budget);
void csv_job_cancel(struct csv_job *job);
void csv_job_free(struct csv_job *job);
unsigned csv_job_progress(const struct csv_job *job); /* 0..1000, DONE is 1000. */
const char *csv_job_phase(const struct csv_job *job);
const char *csv_job_error(const struct csv_job *job);
/* Reusable finite-number scanner. Reset borrows the complete cell text until
 * the next reset/delete. Each step scans at most min(budget,32768) characters;
 * final conversion uses a fixed-size canonical token. Empty fields, incomplete
 * tokens, overflow and libc-reported underflow return -1. Both sort and plot
 * adapters can therefore use the same definition without per-cell allocation.
 */
typedef struct csv_number csv_number;
csv_number *csv_number_create(void);
void csv_number_delete(csv_number *number);
void csv_number_reset(csv_number *number, const char *text);
int csv_number_step(csv_number *number, size_t budget); /* 0 more; 1 valid; -1 invalid. */
double csv_number_value(const csv_number *number);
int csv_number_empty(const csv_number *number); /* Completed token contains only space/tab. */
void csv_free(struct csv_document *doc);
/* Transactional: failure leaves the existing document untouched. */
int csv_load(struct csv_document *doc, const char *path, unsigned delimiter, unsigned header,
             char *error, unsigned capacity);
int csv_parse(struct csv_document *doc, const char *text, uint32_t bytes, unsigned delimiter,
              unsigned header, char *error, unsigned capacity);
void csv_header(struct csv_document *doc, unsigned enabled);
const char *csv_cell(const struct csv_document *doc, uint32_t row, unsigned column);
const char *csv_heading(const struct csv_document *doc, unsigned column);
int csv_sort(struct csv_document *doc, unsigned column, int descending);
void csv_original_order(struct csv_document *doc);
/* TSV with quoting for embedded tabs/newlines/quotes; no silent truncation. */
int csv_copy_row(const struct csv_document *doc, uint32_t row, char *out, unsigned capacity);
#endif
