/* SPDX-License-Identifier: MIT */
#include "jpipe.h"
#include <limits.h>
#include <signal.h>
static void usage(void) {
    fputs("Usage: jpipe [--input jsonl|json|text] [STAGES] [--output jsonl|text]\n"
          "Stages (in order): --map FUNCTION, --filter FUNCTION,\n"
          "                   --reduce FUNCTION --init JSON, --sort COMPARATOR\n"
          "Aliases: jlines, jobj, jmap FUNCTION, jfilter FUNCTION,\n"
          "         jreduce --init JSON FUNCTION, jsort COMPARATOR, jtext\n"
          "Options: -R (text input), -r (text output), --memory CELLS,\n"
          "         --max-record OCTETS, --max-polls COUNT, --max-time-ms MS\n"
          "JSON Lines is the wire format. Functions are synchronous; logging goes\n"
          "to stderr. Sort buffers input in memory; reduce emits once at EOF.\n", stdout);
}
static int stage(JPipe *p, int kind, const char *source, const char *seed) {
    JStage *fresh, *s; size_t n;
    if (p->count == p->capacity) {
        n = p->capacity ? p->capacity * 2 : 4;
        if (n < p->capacity || n > (size_t)-1 / sizeof(*fresh)) return -1;
        fresh = realloc(p->stages, n * sizeof(*fresh)); if (!fresh) return -1;
        p->stages = fresh; p->capacity = n;
    }
    s = &p->stages[p->count++]; memset(s, 0, sizeof(*s));
    s->kind = kind; s->source = source; s->seed = seed;
    s->function = s->accumulator = JS_UNDEFINED; return 0;
}
static int number(const char *text, unsigned long *out) {
    char *end; unsigned long n;
    errno = 0; n = strtoul(text, &end, 10);
    if (errno || !*text || *text == '-' || *end || !n) return -1;
    *out = n; return 0;
}
int main(int argc, char **argv) {
    JPipe p; const char *base, *alias_source = NULL, *initial = NULL, *option, *value;
    int alias = -1, i, kind, status = 2; unsigned long n; size_t at;
    memset(&p, 0, sizeof(p)); p.replacer = JS_UNDEFINED;
    p.memory = 4UL * 1024 * 1024; p.record_limit = 1024UL * 1024;
    p.poll_limit = 100000; p.time_limit = 1000;
    base = strrchr(argv[0], '/'); base = base ? base + 1 : argv[0];
    if (!strcmp(base, "jlines")) p.input = J_TEXT;
    else if (!strcmp(base, "jobj")) p.input = J_DOCUMENT;
    else if (!strcmp(base, "jtext")) p.output = J_TEXT;
    else if (!strcmp(base, "jmap")) alias = J_MAP;
    else if (!strcmp(base, "jfilter")) alias = J_FILTER;
    else if (!strcmp(base, "jreduce")) alias = J_REDUCE;
    else if (!strcmp(base, "jsort")) alias = J_SORT;
    for (i = 1; i < argc; i++) {
        option = argv[i];
        if (!strcmp(option, "--help") || !strcmp(option, "-h")) { usage(); status = 0; goto done; }
        if (!strcmp(option, "--version")) { puts("jpipe 0.1 / QuickJS"); status = 0; goto done; }
        if (!strcmp(option, "-R")) { p.input = J_TEXT; continue; }
        if (!strcmp(option, "-r")) { p.output = J_TEXT; continue; }
        if (option[0] != '-') {
            if (alias < 0 || alias_source) goto invalid;
            alias_source = option; continue;
        }
        value = strchr(option, '=');
        if (value) {
            /* Only the two format switches have an equals spelling. */
            if (!strncmp(option, "--input=", 8)) option = "--input";
            else if (!strncmp(option, "--output=", 9)) option = "--output";
            else goto invalid;
            value++;
        } else { if (++i == argc) goto invalid; value = argv[i]; }
        if (!strcmp(option, "--input")) {
            if (!strcmp(value, "jsonl")) p.input = J_JSONL;
            else if (!strcmp(value, "text")) p.input = J_TEXT;
            else if (!strcmp(value, "json")) p.input = J_DOCUMENT;
            else goto invalid;
        } else if (!strcmp(option, "--output")) {
            if (!strcmp(value, "jsonl")) p.output = J_JSONL;
            else if (!strcmp(value, "text")) p.output = J_TEXT;
            else goto invalid;
        } else if (!strcmp(option, "--init")) {
            if (p.count && p.stages[p.count-1].kind == J_REDUCE && !p.stages[p.count-1].seed)
                p.stages[p.count-1].seed = value;
            else if (!initial) initial = value;
            else goto invalid;
        } else if (!strcmp(option, "--map") || !strcmp(option, "--filter") || !strcmp(option, "--reduce") || !strcmp(option, "--sort")) {
            if (alias >= 0) goto invalid;
            kind = !strcmp(option, "--map") ? J_MAP : !strcmp(option, "--filter") ? J_FILTER : !strcmp(option, "--reduce") ? J_REDUCE : J_SORT;
            if (stage(&p, kind, value, kind == J_REDUCE ? initial : NULL)) goto memory;
            if (kind == J_REDUCE) initial = NULL;
        } else {
            if (number(value, &n)) goto invalid;
            if (!strcmp(option, "--memory")) { if ((size_t)n != n) goto invalid; p.memory = n; }
            else if (!strcmp(option, "--max-record")) { if (n >= (size_t)-1) goto invalid; p.record_limit = n; }
            else if (!strcmp(option, "--max-polls")) p.poll_limit = n;
            else if (!strcmp(option, "--max-time-ms")) p.time_limit = n;
            else goto invalid;
        }
    }
    if (alias >= 0) {
        if (!alias_source) goto invalid;
        if (stage(&p, alias, alias_source, alias == J_REDUCE ? initial : NULL)) goto memory;
        if (alias == J_REDUCE) initial = NULL;
    }
    if (initial) goto invalid;
    for (at = 0; at < p.count; at++) if (p.stages[at].kind == J_REDUCE && !p.stages[at].seed) {
        fputs("jpipe: each reducer requires --init JSON\n", stderr); goto done;
    }
    p.active = p.count;
    /* Conventional Unix pipe cancellation: downstream closure terminates this
     * producer quietly; Ctrl-C remains immediately effective during I/O. */
    signal(SIGPIPE, SIG_DFL);
    status = j_runtime(&p) || j_read(&p) || j_finish(&p) ? 1 : 0;
    goto done;
invalid:
    fputs("jpipe: invalid arguments; see --help\n", stderr); goto done;
memory:
    fputs("jpipe: out of memory parsing stages\n", stderr); status = 1;
done:
    j_dispose(&p); return status;
}
