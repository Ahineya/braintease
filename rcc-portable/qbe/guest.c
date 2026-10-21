/* Portable support for running QBE inside a C16 guest. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/wait.h>
#include <sys/stat.h>

extern long qbe_main(long, char **);
static char *temporary_output;
static int output_complete;
static int output_owned;
#ifndef QBE_INITIAL_SCRATCH
#define QBE_INITIAL_SCRATCH 65536UL
#endif
unsigned long qbe_instruction_capacity = 1UL << 20;
static int retry_scratch;

void qbe_scratch_exhausted(void)
{
    if (retry_scratch && qbe_instruction_capacity < (1UL << 20))
        _exit(125);
    fprintf(stderr, "qbe: too many instructions (maximum %lu)\n", qbe_instruction_capacity);
    exit(1);
}

/* Keep each attempt's scratch address stable: optimizer passes retain interior
 * Ins pointers. A clean fork retries named inputs/output without replaying a
 * stream or publishing partial assembly. No kernel memory extension is needed. */
static int compile_named(int argc, char **argv)
{
    int status = 0, result;
    pid_t child, waited;
    retry_scratch = 1;
    qbe_instruction_capacity = QBE_INITIAL_SCRATCH;
    for (;;) {
        child = fork();
        if (child < 0) {
            fprintf(stderr, "qbe: cannot start compilation attempt\n");
            return 1;
        }
        if (!child) {
            output_owned = 0;
            result = (int)qbe_main(argc, argv);
            exit(result || !output_complete ? 1 : 0);
        }
        do {
            waited = waitpid(child, &status, 0);
        } while (waited < 0 && errno == EINTR);
        if (waited != child || !WIFEXITED(status)) {
            fprintf(stderr, "qbe: compilation attempt failed (wait status %d)\n", status);
            return 1;
        }
        result = WEXITSTATUS(status);
        if (result != 125) {
            output_complete = result == 0;
            return result;
        }
        qbe_instruction_capacity *= 2;
    }
}

static void cleanup_output(void)
{
    if (output_owned)
        unlink(temporary_output);
}

void qbe_output_ready(void)
{
    output_complete = 1;
}

void qbe_assert(const char *expression, const char *file, long line)
{
    fprintf(stderr, "qbe: %s:%ld: assertion failed: %s\n", file, line, expression);
    exit(1);
}

/* The SDK qsort callback returns C int. QBE's compiler indices use long. */
static void swap(char *a, char *b, size_t size)
{
    while (size--) {
        char value = *a;
        *a++ = *b;
        *b++ = value;
    }
}

static void sift(char *base, size_t root, size_t count, size_t size,
                 long (*compare)(const void *, const void *))
{
    while (root < count / 2) {
        size_t child = root * 2 + 1;
        if (child + 1 < count && compare(base + child * size, base + (child + 1) * size) < 0)
            child++;
        if (compare(base + root * size, base + child * size) >= 0)
            return;
        swap(base + root * size, base + child * size, size);
        root = child;
    }
}

void qbe_qsort(void *data, size_t count, size_t size, long (*compare)(const void *, const void *))
{
    char *base = data;
    size_t i;
    if (!size || count < 2)
        return;
    if (count > (size_t)-1 / size)
        qbe_assert("sort span overflow", __FILE__, __LINE__);
    for (i = count / 2; i; i--)
        sift(base, i - 1, count, size, compare);
    for (i = count - 1; i; i--) {
        swap(base, base + i * size, size);
        sift(base, 0, i, size, compare);
    }
}

/* QBE scans only an underscore followed by a floating literal. No general
 * scanf implementation is needed. Preserve the first non-literal character. */
int qbe_scanfloat(FILE *file, void *output, int wide)
{
    char text[256], *end;
    unsigned n = 0;
    int c = getc(file);
    double value;
    if (c != '_') {
        if (c != EOF)
            ungetc(c, file);
        return 0;
    }
    while ((c = getc(file)) != EOF && strchr("0123456789abcdefABCDEFxXpPeE+-.inINftyFTYaN", c)) {
        if (n + 1 == sizeof text)
            qbe_assert("floating literal exceeds 255 cells", __FILE__, __LINE__);
        text[n++] = c;
    }
    if (c != EOF)
        ungetc(c, file);
    text[n] = 0;
    value = strtod(text, &end);
    if (end == text || *end)
        qbe_assert("invalid floating literal", __FILE__, __LINE__);
    if (wide)
        *(double *)output = value;
    else
        *(float *)output = value;
    return 1;
}

int main(int argc, char **argv)
{
    char **arguments = malloc(((size_t)argc + 1) * sizeof(*arguments));
    const char *output = NULL;
    char *attached = NULL;
    int i, result, fd, replayable = 1;
    if (!arguments)
        qbe_assert("argument allocation", __FILE__, __LINE__);
    for (i = 0; i <= argc; i++)
        arguments[i] = argv[i];
    /* Keep QBE's option parsing, but stage named output until generation and
     * close succeed. Parse errors and fatal assertions run the same cleanup. */
    for (i = 1; i < argc; i++) {
        char *option = argv[i];
        if (!strcmp(option, "--")) {
            i++;
            break;
        }
        if (option[0] != '-' || !option[1])
            break;
        if (strchr("dot", option[1])) {
            const char *value = option + 2;
            int argument = i;
            if (!*value) {
                if (++i >= argc)
                    break;
                value = argv[i];
            }
            if (option[1] != 'o') {
                if (option[1] == 'd' || !strcmp(value, "?"))
                    replayable = 0;
                continue;
            }
            if (output)
                qbe_assert("only one output option is supported", __FILE__, __LINE__);
            if (!strcmp(value, "-"))
                continue;
            output = value;
            temporary_output = malloc(strlen(output) + 40);
            if (!temporary_output)
                qbe_assert("output allocation", __FILE__, __LINE__);
            sprintf(temporary_output, "%s.tmp.%u", output, (unsigned)getpid());
            fd = open(temporary_output, O_WRONLY | O_CREAT | O_EXCL, 0600);
            if (fd < 0)
                qbe_assert("cannot create temporary output", __FILE__, __LINE__);
            output_owned = 1;
            atexit(cleanup_output);
            if (close(fd))
                qbe_assert("cannot close temporary output", __FILE__, __LINE__);
            if (argument == i) {
                attached = malloc(strlen(temporary_output) + 3);
                if (!attached)
                    qbe_assert("output option allocation", __FILE__, __LINE__);
                strcpy(attached, "-o");
                strcat(attached, temporary_output);
                arguments[i] = attached;
            } else
                arguments[i] = temporary_output;
        } else
            replayable = 0;
    }
    if (i == argc)
        replayable = 0;
    for (; i < argc; i++) {
        struct stat info;
        if (!strcmp(argv[i], "-") || stat(argv[i], &info) || !S_ISREG(info.st_mode))
            replayable = 0;
    }
    result = output && replayable ? compile_named(argc, arguments) : (int)qbe_main(argc, arguments);
    if (!result && output_complete && output && rename(temporary_output, output)) {
        fprintf(stderr, "qbe: cannot publish output '%s'\n", output);
        result = 1;
    }
    cleanup_output();
    output_owned = 0;
    free(attached);
    free(temporary_output);
    temporary_output = NULL;
    free(arguments);
    return result;
}
