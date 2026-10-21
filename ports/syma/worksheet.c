#include "syma.h"
#include "session.h"
#include <curses.h>
#include "../../libraries/text/text.h"
#include <ctype.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>

#define CELL_LIMIT 512
#define DOCUMENT_LIMIT 4194304UL
/* Source and cached output are owned UTF-8, independent of the term collector. */
typedef struct {
    char *source, *output;
    unsigned note, state; /* 0 stale, 1 evaluated, 2 error */
} Cell;
static struct {
    Cell cells[CELL_LIMIT];
    Count n, limit;
    char *path, *filetext, *savedtext, *temporary_path;
    Buffer input;
    unsigned dirty, interactive, tui;
    SymaContext *context;
} sheet;
static char *duplicate(const char *s) {
    char *p = syma_mem(((Count)strlen(s) + 1) * sizeof(char));
    strcpy(p, s);
    return p;
}
static void invalidate(Count from) {
    Count i;
    for (i = from; i < sheet.n; i++)
        sheet.cells[i].state = 0;
    sheet.dirty = 1;
}
static void clear_sheet(void) {
    Count i;
    if (sheet.context) {
        syma_context_free(sheet.context);
        sheet.context = NULL;
    }
    for (i = 0; i < sheet.n; i++) {
        free(sheet.cells[i].source);
        free(sheet.cells[i].output);
    }
    memset(sheet.cells, 0, sizeof(sheet.cells));
    sheet.n = sheet.dirty = 0;
    free(sheet.path);
    sheet.path = NULL;
}
static void load(const char *path) {
    sheet.filetext = syma_readfile(path);
    if (strlen(sheet.filetext) > DOCUMENT_LIMIT)
        syma_fail("worksheet exceeds 4 MiB");
    Id doc = syma_parse(sheet.filetext);
    if (!syma_headis(doc, "Worksheet") || !syma_is(syma_arg(doc, 0), "V1"))
        syma_fail("expected Worksheet V1");
    Id limit = syma_arg(doc, 1);
    if (!limit || syma_node(limit)->kind != NUM || syma_node(limit)->num < 1 ||
        syma_node(limit)->num > 10000000 || floor(syma_node(limit)->num) != syma_node(limit)->num)
        syma_fail("worksheet needs a positive evaluation limit");
    Count budget = (Count)syma_node(limit)->num;
    Count n = syma_argcnt(doc) - 2, i;
    if (n > CELL_LIMIT)
        syma_fail("worksheet exceeds 512 cells");
    /* Validate the entire document before replacing the open worksheet. */
    for (i = 0; i < n; i++) {
        Id c = syma_arg(doc, i + 2);
        if (!syma_headis(c, "Cell") || syma_argcnt(c) != 3 ||
            !(syma_is(syma_arg(c, 0), "Code") || syma_is(syma_arg(c, 0), "Text")) ||
            syma_node(syma_arg(c, 1))->kind != STR || syma_node(syma_arg(c, 2))->kind != STR ||
            syma_node(syma_arg(c, 1))->len > 1048576UL)
            syma_fail("Cell needs Code|Text, source string and cached output string");
    }
    char *newpath = duplicate(path);
    clear_sheet();
    sheet.path = newpath;
    sheet.limit = budget;
    for (i = 0; i < n; i++) {
        Id c = syma_arg(doc, i + 2);
        sheet.cells[i].note = syma_is(syma_arg(c, 0), "Text");
        sheet.cells[i].source = syma_utf8(syma_arg(c, 1));
        sheet.cells[i].output = syma_utf8(syma_arg(c, 2));
        sheet.n++;
    }
    free(sheet.filetext);
    sheet.filetext = NULL;
}
static void save(const char *path) {
    Buffer b = {0};
    char heading[80];
    snprintf(heading, sizeof(heading), "{Worksheet V1 %lu\n", (unsigned long)sheet.limit);
    syma_buftext(&b, heading);
    Count i;
    for (i = 0; i < sheet.n; i++) {
        Cell *c = &sheet.cells[i];
        syma_buftext(&b, c->note ? "  {Cell Text " : "  {Cell Code ");
        char *source = syma_show(syma_str(c->source), 0);
        syma_buftext(&b, source);
        free(source);
        syma_bufput(&b, ' ');
        char *out = syma_show(syma_str(c->output ? c->output : ""), 0);
        syma_buftext(&b, out);
        free(out);
        syma_buftext(&b, "}\n");
    }
    syma_buftext(&b, "}\n");
    sheet.savedtext = b.p;
    if (b.n > DOCUMENT_LIMIT)
        syma_fail("worksheet exceeds 4 MiB");
    sheet.temporary_path = syma_mem(((Count)strlen(path) + 32) * sizeof(char));
    /* Exclusive creation avoids following a preexisting temporary symlink. */
    snprintf(sheet.temporary_path, strlen(path) + 32, "%s.tmp.%ld", path, (long)getpid());
    int fd = open(sheet.temporary_path, O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (fd < 0)
        syma_fail("cannot create worksheet temporary file");
    FILE *f = fdopen(fd, "w");
    if (!f) {
        close(fd);
        unlink(sheet.temporary_path);
        syma_fail("cannot open worksheet temporary stream");
    }
    int ok = fputs(b.p, f) >= 0;
    if (fclose(f))
        ok = 0;
    if (!ok || rename(sheet.temporary_path, path)) {
        unlink(sheet.temporary_path);
        syma_fail("cannot save worksheet");
    }
    free(sheet.temporary_path);
    sheet.temporary_path = NULL;
    free(sheet.savedtext);
    sheet.savedtext = NULL;
    char *newpath = duplicate(path);
    free(sheet.path);
    sheet.path = newpath;
    sheet.dirty = 0;
    if (!sheet.tui)
        puts("Worksheet saved");
}
static const char *state(Cell *c) {
    return c->note                   ? "Note"
           : c->state == 1           ? "Ready"
           : c->state == 2           ? "Error"
           : c->output && *c->output ? "Needs update"
                                     : "Not run";
}
static void list(void) {
    Count i;
    for (i = 0; i < sheet.n; i++) {
        Cell *c = &sheet.cells[i];
        const char *p = c->source;
        while (isspace((unsigned char)*p))
            p++;
        unsigned char preview[256];
        size_t end = 0, length = strlen(p);
        while (end < length && p[end] != '\n' && p[end] != '\r')
            end++;
        size_t cut = ru_prefix((const unsigned char *)p, end, 56);
        while (cut >= sizeof(preview))
            cut = ru_previous_grapheme((const unsigned char *)p, end, cut);
        memcpy(preview, p, cut);
        preview[cut] = 0;
        printf("%lu [%s] %s%s\n", (unsigned long)i + 1, state(c), preview,
               cut < length ? " ..." : "");
    }
    if (!sheet.n)
        puts("No cells yet. Use :add code or :add text.");
}
static void show(Count from, Count to) {
    Count i;
    for (i = from; i < to; i++) {
        Cell *c = &sheet.cells[i];
        printf("\n[%lu %s]\n%s\n", (unsigned long)i + 1, state(c), c->source);
        if (c->output && *c->output)
            printf("%s%s\n",
                   !c->state       ? "Previous result (needs update): "
                   : c->state == 2 ? "Error: "
                                   : "=> ",
                   c->output);
    }
}
static int ui_progress(Count cell, Count total);
static int worksheet_tui(void);
static int evaluate_cell(Count i) {
    Cell *c = &sheet.cells[i];
    SymaResult result;
    int ok = syma_context_eval(sheet.context, c->source, &result);
    free(c->output);
    c->output = duplicate(ok ? result.text : result.error);
    c->state = ok ? 1 : 2;
    syma_result_free(&result);
    if (!sheet.tui)
        printf("[%lu] %s%s\n", (unsigned long)i + 1, ok ? "=> " : "Error: ", c->output);
    sheet.dirty = 1;
    return ok;
}
/* Individual evaluation uses the live session, just like a REPL. It never
 * replays earlier cells; Run all explicitly rebuilds document-order state. */
static int run_cell(Count i) {
    if (!sheet.context)
        sheet.context = syma_context_new(sheet.limit);
    syma_context_set_limit(sheet.context, sheet.limit);
    int ok = evaluate_cell(i);
    if (ok)
        invalidate(i + 1);
    return ok;
}
static int run(Count to) {
    Count i;
    if (sheet.context)
        syma_context_free(sheet.context);
    sheet.context = syma_context_new(sheet.limit);
    for (i = 0; i < sheet.n; i++)
        sheet.cells[i].state = 0;
    for (i = 0; i < to; i++) {
        Cell *c = &sheet.cells[i];
        if (c->note)
            continue;
        if (sheet.tui && !ui_progress(i, to))
            return -1;
        if (!evaluate_cell(i))
            return 0;
    }
    return 1;
}
static Count cell_index(char **p) {
    char *end;
    while (isspace((unsigned char)**p))
        (*p)++;
    unsigned long n = strtoul(*p, &end, 10);
    if (end == *p || n < 1 || n > sheet.n || (*end && !isspace((unsigned char)*end)))
        syma_fail("cell number out of range");
    *p = end;
    return (Count)n - 1;
}
static void help(void) {
    puts("Syma worksheet\n"
         ":list              list cells\n"
         ":show [N]          show source and results\n"
         ":add code|text     append a cell; end with a lone .\n"
         ":edit N            replace a cell; end with a lone .\n"
         ":remove N          remove a cell\n"
         ":move N M          move a cell to position M\n"
         ":run [N|all]       evaluate from the beginning through N\n"
         ":limit N           set per-expression rewrite limit\n"
         ":save [FILE]       save source and cached results\n"
         ":open FILE         open a worksheet\n"
         ":quit              exit (asks before discarding changes)\n"
         "Code: one term per cell. Import, Module, R and Define add definitions.\n"
         "{Define $name EXPR} stores a value; $name and $it reuse results.\n"
         "In a cell, .. inserts a literal line containing .");
}
static void edit(Count n, unsigned append, unsigned note) {
    char line[1024];
    sheet.input.n = 0;
    if (sheet.input.p)
        sheet.input.p[0] = 0;
    if (sheet.interactive)
        puts("Enter cell source, then . on its own line:");
    unsigned at_start = 1;
    for (;;) {
        if (!fgets(line, sizeof(line), stdin))
            syma_fail("cell cancelled at end of input");
        if (at_start && (!strcmp(line, ".\n") || !strcmp(line, ".\r\n") || !strcmp(line, ".")))
            break;
        if (at_start && (!strcmp(line, "..\n") || !strcmp(line, "..\r\n")))
            memmove(line, line + 1, strlen(line) * sizeof(char));
        syma_buftext(&sheet.input, line);
        if (sheet.input.n > 1048576UL)
            syma_fail("cell exceeds 1 MiB");
        at_start = strchr(line, '\n') != NULL;
    }
    if (!sheet.input.n)
        syma_fail("empty cell discarded");
    if (append) {
        sheet.cells[n].note = note;
        sheet.n++;
    }
    free(sheet.cells[n].source);
    sheet.cells[n].source = duplicate(sheet.input.p);
    invalidate(n);
}
static int discard(void) {
    char answer[32];
    if (!sheet.dirty)
        return 1;
    if (!sheet.interactive)
        return 1;
    fputs("Discard unsaved changes? [y/N] ", stdout);
    fflush(stdout);
    return fgets(answer, sizeof(answer), stdin) && (answer[0] == 'y' || answer[0] == 'Y');
}
int syma_worksheet(int ac, char **av) {
    char line[2048];
    jmp_buf boundary;
    jmp_buf *previous = syma_error_target;
    volatile unsigned failures = 0;
    memset(&sheet, 0, sizeof(sheet));
    sheet.limit = 100000;
    sheet.interactive = isatty(0);
    syma_error_target = &boundary;
    if (setjmp(boundary)) {
        fprintf(stderr, "syma worksheet: %s\n", syma_message);
        failures = 1;
        goto done;
    }
    const char *path = NULL;
    unsigned batch = 0, line_mode = 0;
    int arg;
    for (arg = 2; arg < ac; arg++) {
        if (!strcmp(av[arg], "--run"))
            batch = 1;
        else if (!strcmp(av[arg], "--line"))
            line_mode = 1;
        else if (!strncmp(av[arg], "--", 2) || path)
            syma_fail("usage: syma worksheet [FILE] [--run | --line]");
        else
            path = av[arg];
    }
    if (batch && (!path || line_mode))
        syma_fail("--run requires a worksheet file and cannot use --line");
    if (path)
        load(path);
    if (batch) {
        sheet.interactive = 0;
        failures = !run(sheet.n);
        goto done;
    }
    if (sheet.interactive && isatty(1) && !line_mode &&
        (!getenv("TERM") || strcmp(getenv("TERM"), "dumb"))) {
        failures = worksheet_tui();
        goto done;
    }
    syma_gc(0, 0, NULL, NULL, 0);
    if (sheet.interactive) {
        puts("Syma worksheet — :help for commands");
        list();
    }
    for (;;) {
        if (setjmp(boundary)) {
            fprintf(stderr, "syma worksheet: %s\n", syma_message);
            failures = 1;
            free(sheet.filetext);
            sheet.filetext = NULL;
            free(sheet.savedtext);
            sheet.savedtext = NULL;
            free(sheet.temporary_path);
            sheet.temporary_path = NULL;
        }
        if (sheet.interactive) {
            printf("sheet%s> ", sheet.dirty ? "*" : "");
            fflush(stdout);
        }
        if (!fgets(line, sizeof(line), stdin))
            break;
        Count n = (Count)strlen(line);
        if (n && line[n - 1] != '\n' && !feof(stdin)) {
            int ch;
            while ((ch = getchar()) != EOF && ch != '\n') {
            }
            syma_fail("command line too long");
        }
        while (n && isspace((unsigned char)line[n - 1]))
            line[--n] = 0;
        char *p = line;
        while (isspace((unsigned char)*p))
            p++;
        if (!*p)
            continue;
        char *op = p;
        while (*p && !isspace((unsigned char)*p))
            p++;
        if (*p)
            *p++ = 0;
        while (isspace((unsigned char)*p))
            p++;
        if (!strcmp(op, ":help"))
            help();
        else if (!strcmp(op, ":list"))
            list();
        else if (!strcmp(op, ":show")) {
            if (*p) {
                Count i = cell_index(&p);
                show(i, i + 1);
            } else
                show(0, sheet.n);
        } else if (!strcmp(op, ":add")) {
            if (sheet.n == CELL_LIMIT)
                syma_fail("worksheet exceeds 512 cells");
            if (strcmp(p, "code") && strcmp(p, "text"))
                syma_fail("usage: :add code|text");
            edit(sheet.n, 1, !strcmp(p, "text"));
        } else if (!strcmp(op, ":edit")) {
            Count i = cell_index(&p);
            show(i, i + 1);
            edit(i, 0, 0);
        } else if (!strcmp(op, ":remove")) {
            Count i = cell_index(&p);
            free(sheet.cells[i].source);
            free(sheet.cells[i].output);
            memmove(sheet.cells + i, sheet.cells + i + 1, (sheet.n - i - 1) * sizeof(Cell));
            memset(sheet.cells + --sheet.n, 0, sizeof(Cell));
            invalidate(i);
        } else if (!strcmp(op, ":move")) {
            Count i = cell_index(&p), j = cell_index(&p), k;
            Cell c = sheet.cells[i];
            if (i < j)
                for (k = i; k < j; k++)
                    sheet.cells[k] = sheet.cells[k + 1];
            else
                for (k = i; k > j; k--)
                    sheet.cells[k] = sheet.cells[k - 1];
            sheet.cells[j] = c;
            invalidate(i < j ? i : j);
        } else if (!strcmp(op, ":run")) {
            Count to = !*p || !strcmp(p, "all") ? sheet.n : cell_index(&p) + 1;
            if (!run(to))
                failures = 1;
        } else if (!strcmp(op, ":limit")) {
            char *end;
            unsigned long limit = strtoul(p, &end, 10);
            if (!*p || *end || !limit || limit > 10000000UL)
                syma_fail("limit must be between 1 and 10000000");
            sheet.limit = (Count)limit;
            invalidate(0);
        } else if (!strcmp(op, ":save")) {
            if (!*p && !sheet.path)
                syma_fail("usage: :save FILE");
            save(*p ? p : sheet.path);
        } else if (!strcmp(op, ":open")) {
            if (!*p)
                syma_fail("usage: :open FILE");
            if (discard())
                load(p);
        } else if (!strcmp(op, ":quit")) {
            if (discard())
                break;
        } else
            syma_fail("unknown worksheet command; use :help");
        syma_gc(0, 0, NULL, NULL, 0);
    }
done:
    clear_sheet();
    free(sheet.input.p);
    free(sheet.filetext);
    free(sheet.savedtext);
    free(sheet.temporary_path);
    syma_error_target = previous;
    return failures ? 1 : 0;
}

#include "worksheet-tui.inc"
