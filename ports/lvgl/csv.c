/* SPDX-License-Identifier: MIT */
#include "csv.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <ctype.h>
#include <float.h>

static int fail(char *error, unsigned capacity, const char *message) {
    if (capacity)
        snprintf(error, capacity, "%s", message);
    return -1;
}
void csv_free(struct csv_document *doc) {
    free(doc->data);
    free(doc->fields);
    free(doc->records);
    free(doc->order);
    memset(doc, 0, sizeof(*doc));
    doc->sort_column = -1;
}
static const char *raw_cell(const struct csv_document *d, uint32_t row, unsigned col) {
    if (row >= d->record_count || col >= d->records[row].count)
        return "";
    return d->data + d->fields[d->records[row].first + col];
}
void csv_original_order(struct csv_document *d) {
    uint32_t i;
    for (i = 0; i < d->row_count; i++)
        d->order[i] = i + (d->header && d->record_count ? 1 : 0);
    d->sort_column = -1;
    d->descending = 0;
}
void csv_header(struct csv_document *d, unsigned enabled) {
    d->header = !!enabled;
    d->row_count = d->record_count - (d->header && d->record_count ? 1 : 0);
    csv_original_order(d);
}
const char *csv_cell(const struct csv_document *d, uint32_t row, unsigned col) {
    return row < d->row_count ? raw_cell(d, d->order[row], col) : "";
}
const char *csv_heading(const struct csv_document *d, unsigned col) {
    return d->header && d->record_count ? raw_cell(d, 0, col) : "";
}

/* Every potentially long scan has a saved cursor. Exact counting before decoding
 * avoids repeated realloc/copies of field and record arrays. */
enum job_phase {
    J_READ, J_COPY, J_VALIDATE, J_DETECT, J_COUNT, J_DECODE, J_INDEX,
    J_KEYS, J_MERGE
};
enum number_phase {
    N_LEAD, N_SIGN, N_DEC, N_FRACTION, N_EXP, N_EXP_SIGN, N_EXP_DIGITS,
    N_TRAIL, N_HEX, N_HEX_FRACTION, N_HEX_EXP, N_HEX_EXP_SIGN, N_HEX_EXP_DIGITS,
    N_INVALID
};
#define NUMBER_DIGITS 800
struct csv_number {
    const char *text;
    uint32_t offset;
    int status;
    double value;
    unsigned number_phase, negative, exp_negative, digits, stored, sticky, nonempty, hex;
    int32_t before;
    uint32_t leading;
    long exponent;
    char significant[NUMBER_DIGITS + 1];
};
struct csv_job {
    struct csv_document *target, next;
    FILE *file;
    const char *source;
    int status, phase;
    char error[180];
    uint32_t size, at, r, w, start, first, expected_fields, expected_records;
    unsigned delimiter, header, quoted, closed, started, pending, count;
    unsigned utf_left, utf_min, utf_max, detect_quote;
    uint32_t commas, tabs, semis;
    unsigned column, descending, numeric;
    uint32_t *left, *right, key_at, width, base, l, middle, rr, end, out;
    uint32_t compare_at;
    double *keys;
    unsigned char *empty;
    unsigned key_active, merging;
    struct csv_number number;
};
static int job_fail(struct csv_job *j, const char *reason) {
    snprintf(j->error, sizeof(j->error), "%s", reason);
    j->status = CSV_JOB_ERROR;
    if (j->file) {
        fclose(j->file);
        j->file = NULL;
    }
    return j->status;
}
static int record_fail(struct csv_job *j, const char *reason) {
    snprintf(j->error, sizeof(j->error), "Record %lu: %s",
             (unsigned long)j->next.record_count + 1, reason);
    j->status = CSV_JOB_ERROR;
    return j->status;
}
static struct csv_job *new_job(struct csv_document *doc, unsigned delimiter, unsigned header,
                                char *error, unsigned capacity) {
    struct csv_job *j;
    if (delimiter && delimiter != ',' && delimiter != '\t' && delimiter != ';') {
        fail(error, capacity, "Unsupported delimiter.");
        return NULL;
    }
    j = calloc(1, sizeof(*j));
    if (!j) {
        fail(error, capacity, "Out of memory.");
        return NULL;
    }
    j->target = doc;
    j->delimiter = delimiter;
    j->header = !!header;
    j->next.sort_column = -1;
    return j;
}
struct csv_job *csv_load_begin(struct csv_document *doc, const char *path, unsigned delimiter,
                               unsigned header, char *error, unsigned capacity) {
    struct csv_job *j = new_job(doc, delimiter, header, error, capacity);
    long size;
    if (!j)
        return NULL;
    j->file = fopen(path, "rb");
    if (!j->file) {
        if (capacity)
            snprintf(error, capacity, "Cannot open file: %s", strerror(errno));
        csv_job_free(j);
        return NULL;
    }
    if (fseek(j->file, 0, SEEK_END) || (size = ftell(j->file)) < 0 ||
        fseek(j->file, 0, SEEK_SET)) {
        fail(error, capacity, "Cannot determine file size.");
        csv_job_free(j);
        return NULL;
    }
    if ((unsigned long)size > CSV_MAX_BYTES) {
        fail(error, capacity, "The file exceeds 4 MiB.");
        csv_job_free(j);
        return NULL;
    }
    j->size = (uint32_t)size;
    j->next.data = malloc(j->size + 2UL);
    if (!j->next.data) {
        fail(error, capacity, "Out of memory.");
        csv_job_free(j);
        return NULL;
    }
    j->phase = J_READ;
    return j;
}
struct csv_job *csv_parse_begin(struct csv_document *doc, const char *text, uint32_t bytes,
                                unsigned delimiter, unsigned header, char *error,
                                unsigned capacity) {
    struct csv_job *j;
    if (bytes > CSV_MAX_BYTES) {
        fail(error, capacity, "The file exceeds 4 MiB.");
        return NULL;
    }
    j = new_job(doc, delimiter, header, error, capacity);
    if (!j)
        return NULL;
    j->next.data = malloc(bytes + 2UL);
    if (!j->next.data) {
        fail(error, capacity, "Out of memory.");
        csv_job_free(j);
        return NULL;
    }
    j->source = text;
    j->size = bytes;
    j->phase = J_COPY;
    return j;
}
void csv_job_cancel(struct csv_job *j) {
    if (j && j->status == CSV_JOB_RUNNING) {
        j->status = CSV_JOB_CANCELLED;
        if (j->file) {
            fclose(j->file);
            j->file = NULL;
        }
    }
}
void csv_job_free(struct csv_job *j) {
    if (!j)
        return;
    if (j->file)
        fclose(j->file);
    csv_free(&j->next);
    free(j->left);
    free(j->right);
    free(j->keys);
    free(j->empty);
    free(j);
}
const char *csv_job_error(const struct csv_job *j) {
    return j ? j->error : "Cannot create job.";
}
const char *csv_job_phase(const struct csv_job *j) {
    if (!j)
        return "Error";
    if (j->status == CSV_JOB_CANCELLED)
        return "Cancelled";
    if (j->status == CSV_JOB_ERROR)
        return "Error";
    if (j->status == CSV_JOB_DONE)
        return "Done";
    switch (j->phase) {
    case J_READ: return "Reading";
    case J_COPY: return "Copying";
    case J_VALIDATE: return "Validating UTF-8";
    case J_DETECT: return "Detecting delimiter";
    case J_COUNT: return "Indexing records";
    case J_DECODE: return "Decoding fields";
    case J_INDEX: return "Preparing rows";
    case J_KEYS: return "Reading sort keys";
    default: return "Sorting";
    }
}
unsigned csv_job_progress(const struct csv_job *j) {
    uint32_t n;
    if (!j)
        return 0;
    if (j->status == CSV_JOB_DONE)
        return 1000;
    if (j->phase == J_KEYS)
        return (unsigned)(j->key_at * 200UL / (j->target->row_count ? j->target->row_count : 1));
    if (j->phase == J_MERGE) {
        uint32_t width = 1, passes = 0, pass = 0;
        n = j->target->row_count;
        while (width < n) {
            if (width < j->width)
                pass++;
            width *= 2;
            passes++;
        }
        return passes ? 200 + (unsigned)((pass * 800UL +
            j->out * 800UL / (n ? n : 1)) / passes) : 999;
    }
    n = j->size ? j->size : 1;
    switch (j->phase) {
    case J_READ: case J_COPY: return (unsigned)(j->at * 150UL / n);
    case J_VALIDATE: return 150 + (unsigned)(j->at * 150UL / n);
    case J_DETECT: return 300 + (unsigned)(j->at * 100UL / n);
    case J_COUNT: return 400 + (unsigned)(j->r * 250UL / n);
    case J_DECODE: return 650 + (unsigned)(j->r * 250UL / n);
    default: return 900 + (unsigned)(j->at * 99UL /
        (j->next.row_count ? j->next.row_count : 1));
    }
}
static void parser_reset(struct csv_job *j) {
    j->r = j->size >= 3 && (unsigned char)j->next.data[0] == 239 &&
        (unsigned char)j->next.data[1] == 187 && (unsigned char)j->next.data[2] == 191 ? 3 : 0;
    j->w = j->start = j->first = 0;
    j->quoted = j->closed = j->started = j->pending = j->count = 0;
    j->next.record_count = j->next.field_count = 0;
}
/* One input byte (plus at most one escaped quote / CRLF byte), or EOF. */
static int parser_byte(struct csv_job *j) {
    struct csv_document *d = &j->next;
    unsigned c = j->r < j->size ? (unsigned char)d->data[j->r++] : 0;
    unsigned write = j->phase == J_DECODE;
    if (j->quoted) {
        if (!c)
            return record_fail(j, "Unclosed quoted field.");
        if (c == '"') {
            if (j->r < j->size && d->data[j->r] == '"') {
                j->r++;
                if (write)
                    d->data[j->w] = '"';
                j->w++;
            } else {
                j->quoted = 0;
                j->closed = 1;
            }
        } else {
            if (write)
                d->data[j->w] = c;
            j->w++;
        }
        return 0;
    }
    if (!j->closed && !j->started && c == '"') {
        j->quoted = 1;
        j->started = j->pending = 1;
        return 0;
    }
    if (c == j->delimiter || c == '\r' || c == '\n' || !c) {
        if (d->field_count == CSV_MAX_FIELDS)
            return record_fail(j, "The file exceeds one million cells.");
        if (j->count == CSV_MAX_COLUMNS)
            return record_fail(j, "The file exceeds 64 columns.");
        if (write) {
            d->fields[d->field_count] = j->start;
            d->data[j->w] = 0;
        }
        d->field_count++;
        j->start = ++j->w;
        j->count++;
        j->closed = j->started = 0;
        if (c == j->delimiter) {
            j->pending = 1;
            return 0;
        }
        if (c == '\r' && j->r < j->size && d->data[j->r] == '\n')
            j->r++;
        if (d->record_count == CSV_MAX_ROWS + 1UL)
            return record_fail(j, "The file exceeds 100,000 data rows.");
        if (write) {
            d->records[d->record_count].first = j->first;
            d->records[d->record_count].count = j->count;
        }
        d->record_count++;
        if (j->count > d->columns)
            d->columns = j->count;
        j->first = d->field_count;
        j->count = j->pending = 0;
        return 0;
    }
    if (j->closed)
        return record_fail(j, "Unexpected text after a closing quote.");
    if (c == '"')
        return record_fail(j, "A quote inside an unquoted field must be escaped in a quoted field.");
    j->started = j->pending = 1;
    if (write)
        d->data[j->w] = c;
    j->w++;
    return 0;
}
/* Scan arbitrary-length numeric fields without handing strtod an unbounded
 * string. 800 significant decimal/hex digits plus a sticky digit exceed the
 * precision needed to distinguish every finite IEEE double rounding boundary.
 * The canonical token given to libc is always under 840 characters. */
void csv_number_reset(csv_number *n, const char *text) {
    if (!n)
        return;
    n->text = text ? text : "";
    n->offset = 0;
    n->status = 0;
    n->value = 0.0;
    n->number_phase = N_LEAD;
    n->negative = n->exp_negative = n->digits = n->stored = n->sticky = 0;
    n->nonempty = n->hex = 0;
    n->before = 0;
    n->leading = 0;
    n->exponent = 0;
}
static void number_digit(struct csv_number *n, unsigned c, unsigned value, int before) {
    n->digits = 1;
    if (before)
        n->before++;
    if (!n->stored && !value)
        n->leading++;
    else if (n->stored < NUMBER_DIGITS)
        n->significant[n->stored++] = c;
    else if (value)
        n->sticky = 1;
}
static unsigned hex_value(unsigned c) {
    return c >= '0' && c <= '9' ? c - '0' :
        c >= 'a' && c <= 'f' ? c - 'a' + 10 :
        c >= 'A' && c <= 'F' ? c - 'A' + 10 : 16;
}
static void number_feed(struct csv_number *n, unsigned c) {
    unsigned phase = n->number_phase, value;
    if (c != ' ' && c != '\t')
        n->nonempty = 1;
    if (phase == N_INVALID)
        return;
    if (phase == N_LEAD) {
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f')
            return;
        if (c == '+' || c == '-') {
            n->negative = c == '-';
            n->number_phase = N_SIGN;
            return;
        }
        phase = N_SIGN;
    }
    if (phase == N_SIGN) {
        n->number_phase = c == '.' ? N_FRACTION : N_DEC;
        if (c == '.')
            return;
        phase = N_DEC;
    }
    if (phase == N_DEC || phase == N_FRACTION) {
        if (c >= '0' && c <= '9') {
            number_digit(n, c, c - '0', phase == N_DEC);
            return;
        }
        if (phase == N_DEC && (c == 'x' || c == 'X') &&
            n->before == 1 && n->leading == 1 && !n->stored) {
            n->number_phase = N_HEX;
            n->hex = 1;
            n->digits = 0;
            n->before = n->leading = 0;
            return;
        }
        if (phase == N_DEC && c == '.') {
            n->number_phase = N_FRACTION;
            return;
        }
        if ((c == 'e' || c == 'E') && n->digits) {
            n->number_phase = N_EXP;
            return;
        }
    } else if (phase == N_HEX || phase == N_HEX_FRACTION) {
        value = hex_value(c);
        if (value < 16) {
            number_digit(n, c, value, phase == N_HEX);
            return;
        }
        if (phase == N_HEX && c == '.') {
            n->number_phase = N_HEX_FRACTION;
            return;
        }
        if ((c == 'p' || c == 'P') && n->digits) {
            n->number_phase = N_HEX_EXP;
            return;
        }
    } else if (phase == N_EXP || phase == N_HEX_EXP) {
        if (c == '+' || c == '-') {
            n->exp_negative = c == '-';
            n->number_phase = phase == N_EXP ? N_EXP_SIGN : N_HEX_EXP_SIGN;
            return;
        }
        phase = phase == N_EXP ? N_EXP_SIGN : N_HEX_EXP_SIGN;
    }
    if (phase == N_EXP_SIGN || phase == N_HEX_EXP_SIGN ||
        phase == N_EXP_DIGITS || phase == N_HEX_EXP_DIGITS) {
        if (c >= '0' && c <= '9') {
            n->number_phase = n->hex ? N_HEX_EXP_DIGITS : N_EXP_DIGITS;
            if (n->exponent < 10000000L) {
                n->exponent = n->exponent * 10 + c - '0';
                if (n->exponent > 10000000L)
                    n->exponent = 10000000L;
            }
            return;
        }
        if (phase == N_EXP_SIGN || phase == N_HEX_EXP_SIGN) {
            n->number_phase = N_INVALID;
            return;
        }
    }
    if ((c == ' ' || c == '\t') && n->digits)
        n->number_phase = N_TRAIL;
    else
        n->number_phase = N_INVALID;
}
static int number_finish(struct csv_number *n, double *value) {
    char token[NUMBER_DIGITS + 40], *end;
    unsigned at = 0, i, digits = n->stored;
    long exponent;
    if (!n->nonempty)
        return 0;
    if (!n->digits || n->number_phase == N_INVALID || n->number_phase == N_EXP ||
        n->number_phase == N_EXP_SIGN || n->number_phase == N_HEX_EXP ||
        n->number_phase == N_HEX_EXP_SIGN)
        return -1;
    if (!digits) {
        *value = n->negative ? -0.0 : 0.0;
        return 1;
    }
    if (n->negative)
        token[at++] = '-';
    if (n->hex) {
        token[at++] = '0';
        token[at++] = 'x';
    }
    for (i = 0; i < digits; i++)
        token[at++] = n->significant[i];
    if (n->sticky) {
        token[at++] = '1';
        digits++;
    }
    exponent = (long)n->before - (long)n->leading - (long)digits;
    if (n->hex)
        exponent *= 4;
    exponent += n->exp_negative ? -n->exponent : n->exponent;
    snprintf(token + at, sizeof(token) - at, "%c%ld", n->hex ? 'p' : 'e', exponent);
    errno = 0;
    *value = strtod(token, &end);
    return *end || errno == ERANGE || *value != *value ||
        *value > DBL_MAX || *value < -DBL_MAX ? -1 : 1;
}
csv_number *csv_number_create(void) {
    csv_number *n = malloc(sizeof(*n));
    if (n)
        csv_number_reset(n, "");
    return n;
}
void csv_number_delete(csv_number *n) { free(n); }
static int number_step(csv_number *n, size_t *budget) {
    while (*budget && !n->status) {
        if (n->text[n->offset])
            number_feed(n, (unsigned char)n->text[n->offset++]);
        else
            n->status = number_finish(n, &n->value) == 1 ? 1 : -1;
        (*budget)--;
    }
    return n->status;
}
int csv_number_step(csv_number *n, size_t budget) {
    if (!n)
        return -1;
    if (budget > 32768UL)
        budget = 32768UL;
    return number_step(n, &budget);
}
double csv_number_value(const csv_number *n) { return n && n->status == 1 ? n->value : 0.0; }
int csv_number_empty(const csv_number *n) { return n && n->status == -1 && !n->nonempty; }
struct csv_job *csv_sort_begin(struct csv_document *doc, unsigned column, int descending,
                               char *error, unsigned capacity) {
    struct csv_job *j;
    uint32_t records = doc->record_count ? doc->record_count : 1;
    if (column >= doc->columns) {
        fail(error, capacity, "Invalid sort column.");
        return NULL;
    }
    j = new_job(doc, 0, 0, error, capacity);
    if (!j)
        return NULL;
    /* Preserve csv_header's ability to expose a former heading as data. */
    j->left = malloc(records * sizeof(*j->left));
    j->right = malloc(records * sizeof(*j->right));
    j->keys = malloc(records * sizeof(*j->keys));
    j->empty = malloc(records * sizeof(*j->empty));
    if (!j->left || !j->right || !j->keys || !j->empty) {
        fail(error, capacity, "Out of memory.");
        csv_job_free(j);
        return NULL;
    }
    j->column = column;
    j->descending = !!descending;
    j->numeric = 1;
    j->width = 1;
    j->phase = J_KEYS;
    return j;
}
static int merge_step(struct csv_job *j, size_t *budget) {
    const struct csv_document *d = j->target;
    uint32_t n = d->row_count;
    while (*budget) {
        if (j->width >= n) {
            uint32_t *old = j->target->order;
            j->target->order = j->left;
            j->left = NULL;
            free(old);
            j->target->sort_column = j->column;
            j->target->descending = j->descending;
            return j->status = CSV_JOB_DONE;
        }
        if (j->base >= n) {
            uint32_t *swap = j->left;
            j->left = j->right;
            j->right = swap;
            j->width *= 2;
            j->base = j->out = j->merging = 0;
            continue;
        }
        if (!j->merging) {
            j->l = j->base;
            j->middle = j->base + j->width < n ? j->base + j->width : n;
            j->rr = j->middle;
            j->end = j->base + j->width * 2 < n ? j->base + j->width * 2 : n;
            j->out = j->base;
            j->merging = 1;
        }
        if (j->l >= j->middle && j->rr >= j->end) {
            j->base += j->width * 2;
            j->merging = 0;
            continue;
        }
        if (j->l >= j->middle || j->rr >= j->end) {
            j->right[j->out++] = j->l < j->middle ? j->left[j->l++] : j->left[j->rr++];
            (*budget)--;
        } else {
            uint32_t a = j->left[j->l], b = j->left[j->rr];
            int cmp;
            if (j->empty[a] != j->empty[b])
                cmp = j->empty[a] ? 1 : -1;
            else {
                if (j->empty[a])
                    cmp = 0;
                else if (j->numeric)
                    cmp = j->keys[a] < j->keys[b] ? -1 : j->keys[a] > j->keys[b] ? 1 : 0;
                else {
                    const unsigned char *sa = (const unsigned char *)raw_cell(d, a, j->column);
                    const unsigned char *sb = (const unsigned char *)raw_cell(d, b, j->column);
                    unsigned ca = sa[j->compare_at], cb = sb[j->compare_at];
                    (*budget)--;
                    if (ca == cb && ca) {
                        j->compare_at++;
                        continue;
                    }
                    cmp = ca < cb ? -1 : ca > cb ? 1 : 0;
                    j->compare_at = 0;
                    /* Publishing this comparison's output is a constant extra
                     * unit; do it now even if the last character used budget. */
                    (*budget)++;
                }
                if (j->descending)
                    cmp = cmp < 0 ? 1 : cmp > 0 ? -1 : 0;
            }
            j->right[j->out++] = cmp <= 0 ? j->left[j->l++] : j->left[j->rr++];
            (*budget)--;
        }
    }
    return CSV_JOB_RUNNING;
}
int csv_job_step(struct csv_job *j, size_t budget) {
    if (!j)
        return CSV_JOB_ERROR;
    if (j->status != CSV_JOB_RUNNING || !budget)
        return j->status;
    if (budget > 32768UL)
        budget = 32768UL;
    while (budget && j->status == CSV_JOB_RUNNING) {
        if (j->phase == J_READ || j->phase == J_COPY) {
            uint32_t amount = j->size - j->at;
            if (amount > budget)
                amount = budget;
            if (amount > 4096UL)
                amount = 4096UL;
            if (amount) {
                if (j->phase == J_READ) {
                    if (fread(j->next.data + j->at, 1, amount, j->file) != amount || ferror(j->file))
                        return job_fail(j, "Cannot read file.");
                } else
                    memcpy(j->next.data + j->at, j->source + j->at, amount);
                j->at += amount;
                budget -= amount;
            }
            if (j->at == j->size) {
                if (j->file) {
                    fclose(j->file);
                    j->file = NULL;
                }
                j->source = NULL;
                j->next.data[j->size] = 0;
                j->at = 0;
                j->phase = J_VALIDATE;
            }
        } else if (j->phase == J_VALIDATE) {
            if (j->at == j->size) {
                if (j->utf_left)
                    return job_fail(j, "The file must be UTF-8 text without NUL characters.");
                j->at = 0;
                if (j->delimiter) {
                    j->next.delimiter = j->delimiter;
                    j->phase = J_COUNT;
                    parser_reset(j);
                } else
                    j->phase = J_DETECT;
                continue;
            }
            {
                unsigned c = (unsigned char)j->next.data[j->at++];
                budget--;
                if (j->utf_left) {
                    if (c < j->utf_min || c > j->utf_max)
                        return job_fail(j, "The file must be UTF-8 text without NUL characters.");
                    j->utf_left--;
                    j->utf_min = 128;
                    j->utf_max = 191;
                } else if (!c)
                    return job_fail(j, "The file must be UTF-8 text without NUL characters.");
                else if (c >= 128) {
                    j->utf_min = 128;
                    j->utf_max = 191;
                    if (c >= 194 && c <= 223)
                        j->utf_left = 1;
                    else if (c >= 224 && c <= 239) {
                        j->utf_left = 2;
                        if (c == 224) j->utf_min = 160;
                        if (c == 237) j->utf_max = 159;
                    } else if (c >= 240 && c <= 244) {
                        j->utf_left = 3;
                        if (c == 240) j->utf_min = 144;
                        if (c == 244) j->utf_max = 143;
                    } else
                        return job_fail(j, "The file must be UTF-8 text without NUL characters.");
                }
            }
        } else if (j->phase == J_DETECT) {
            unsigned c = j->at < j->size ? (unsigned char)j->next.data[j->at++] : 0;
            budget--;
            if (c == '"') {
                if (j->detect_quote && j->at < j->size && j->next.data[j->at] == '"')
                    j->at++;
                else
                    j->detect_quote = !j->detect_quote;
            } else if (!j->detect_quote) {
                if (c == ',') j->commas++;
                if (c == '\t') j->tabs++;
                if (c == ';') j->semis++;
            }
            if (!c || (!j->detect_quote && (c == '\r' || c == '\n'))) {
                j->delimiter = j->tabs > j->commas && j->tabs >= j->semis ? '\t' :
                    j->semis > j->commas ? ';' : ',';
                j->next.delimiter = j->delimiter;
                j->phase = J_COUNT;
                parser_reset(j);
            }
        } else if (j->phase == J_COUNT || j->phase == J_DECODE) {
            if (j->r >= j->size && !j->pending && !j->count) {
                if (j->phase == J_COUNT) {
                    j->next.row_count = j->next.record_count -
                        (j->header && j->next.record_count ? 1 : 0);
                    if (j->next.row_count > CSV_MAX_ROWS)
                        return record_fail(j, "The file exceeds 100,000 data rows.");
                    j->expected_fields = j->next.field_count;
                    j->expected_records = j->next.record_count;
                    j->next.fields = malloc((j->expected_fields ? j->expected_fields : 1) *
                        sizeof(*j->next.fields));
                    j->next.records = malloc((j->expected_records ? j->expected_records : 1) *
                        sizeof(*j->next.records));
                    /* Header can be toggled later without replacing storage. */
                    j->next.order = malloc((j->expected_records ? j->expected_records : 1) *
                        sizeof(*j->next.order));
                    if (!j->next.fields || !j->next.records || !j->next.order)
                        return job_fail(j, "Out of memory.");
                    parser_reset(j);
                    j->phase = J_DECODE;
                } else {
                    j->next.bytes = j->size;
                    j->next.header = j->header;
                    j->at = 0;
                    j->phase = J_INDEX;
                }
                continue;
            }
            if (parser_byte(j))
                return j->status;
            budget--;
        } else if (j->phase == J_INDEX) {
            if (j->at < j->next.row_count) {
                j->next.order[j->at] = j->at + (j->header && j->next.record_count ? 1 : 0);
                j->at++;
                budget--;
            } else {
                csv_free(j->target);
                *j->target = j->next;
                memset(&j->next, 0, sizeof(j->next));
                j->next.sort_column = -1;
                return j->status = CSV_JOB_DONE;
            }
        } else if (j->phase == J_KEYS) {
            if (j->key_at == j->target->row_count) {
                j->phase = J_MERGE;
                continue;
            }
            if (!j->key_active) {
                j->left[j->key_at] = j->target->order[j->key_at];
                csv_number_reset(&j->number,
                    raw_cell(j->target, j->left[j->key_at], j->column));
                j->key_active = 1;
            }
            {
                int kind = number_step(&j->number, &budget);
                if (kind) {
                    uint32_t row = j->left[j->key_at];
                    j->keys[row] = csv_number_value(&j->number);
                    j->empty[row] = csv_number_empty(&j->number);
                    if (kind < 0 && !j->empty[row])
                        j->numeric = 0;
                    j->key_at++;
                    j->key_active = 0;
                }
            }
        } else
            return merge_step(j, &budget);
    }
    return j->status;
}
static int finish_sync(struct csv_job *j, char *error, unsigned capacity) {
    int status;
    if (!j)
        return -1;
    do {
        status = csv_job_step(j, 32768UL);
    } while (status == CSV_JOB_RUNNING);
    if (status != CSV_JOB_DONE)
        fail(error, capacity, csv_job_error(j));
    csv_job_free(j);
    return status == CSV_JOB_DONE ? 0 : -1;
}
int csv_load(struct csv_document *doc, const char *path, unsigned delimiter, unsigned header,
             char *error, unsigned capacity) {
    return finish_sync(csv_load_begin(doc, path, delimiter, header, error, capacity), error, capacity);
}
int csv_parse(struct csv_document *doc, const char *text, uint32_t bytes, unsigned delimiter,
              unsigned header, char *error, unsigned capacity) {
    return finish_sync(csv_parse_begin(doc, text, bytes, delimiter, header, error, capacity),
                       error, capacity);
}
int csv_sort(struct csv_document *doc, unsigned column, int descending) {
    char error[180];
    return finish_sync(csv_sort_begin(doc, column, descending, error, sizeof(error)),
                       error, sizeof(error));
}
int csv_copy_row(const struct csv_document *d, uint32_t row, char *out, unsigned capacity) {
    unsigned col, at = 0;
    if (row >= d->row_count || !capacity)
        return -1;
    for (col = 0; col < d->columns; col++) {
        const char *s = csv_cell(d, row, col);
        unsigned quote = strpbrk(s, "\t\r\n\"") != NULL;
        if (col) {
            if (at + 1 >= capacity)
                return -1;
            out[at++] = '\t';
        }
        if (quote) {
            if (at + 1 >= capacity)
                return -1;
            out[at++] = '"';
        }
        for (; *s; s++) {
            if (at + 2 >= capacity)
                return -1;
            if (quote && *s == '"')
                out[at++] = '"';
            out[at++] = *s;
        }
        if (quote) {
            if (at + 1 >= capacity)
                return -1;
            out[at++] = '"';
        }
    }
    out[at] = 0;
    return 0;
}
