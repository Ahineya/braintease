/* UNIX V7 source code: see /COPYRIGHT or www.tuhs.org for details. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Ripple adaptation: validated long counts, bounded names, checked output. */
#include <errno.h>
#include <sys/stat.h>
static unsigned long number(const char *s) {
    char *end;
    unsigned long n;
    errno = 0; n = strtoul(s, &end, 10);
    if (errno || end == s || *end || *s == '-' || !n || n > 2147483647UL) {
        fputs("split: invalid line count\n", stderr); exit(1);
    }
    return n;
}
int main(int argc, const char *argv[]) {
    unsigned long limit = 1000, lines = 0;
    int i = 1, part = 0, c, operands = 0;
    const char *input = NULL, *prefix = "x";
    char name[512];
    FILE *in = stdin, *out = NULL;
    struct stat source, target;
    int have_source;
    for (; i < argc; i++) {
        if (!strcmp(argv[i], "--")) { i++; break; }
        if (!strcmp(argv[i], "-l")) {
            if (++i == argc) { fputs("split: missing count\n", stderr); return 1; }
            limit = number(argv[i]);
        } else if (argv[i][0] == '-' && argv[i][1] >= '0' && argv[i][1] <= '9') limit = number(argv[i] + 1);
        else break;
    }
    for (; i < argc; i++) {
        if (++operands == 1) input = argv[i];
        else if (operands == 2) prefix = argv[i];
        else { fputs("usage: split [-l lines|-lines] [file|- [prefix]]\n", stderr); return 1; }
    }
    if (strlen(prefix) > sizeof(name) - 3) { fputs("split: prefix too long\n", stderr); return 1; }
    if (input && strcmp(input, "-") && !(in = fopen(input, "r"))) { perror(input); return 1; }
    have_source = fstat(fileno(in), &source) == 0;
    while ((c = getc(in)) != EOF) {
        if (!out) {
            if (part == 676) { fputs("split: exhausted aa..zz suffixes\n", stderr); return 1; }
            sprintf(name, "%s%c%c", prefix, 'a' + part / 26, 'a' + part % 26);
            part++;
            if (have_source && !stat(name, &target) && target.st_ino == source.st_ino && target.st_dev == source.st_dev) {
                fputs("split: output would overwrite input\n", stderr); return 1;
            }
            out = fopen(name, "w");
            if (!out) { perror(name); return 1; }
        }
        if (putc(c, out) == EOF) { perror(name); return 1; }
        if (c == '\n' && ++lines == limit) {
            if (fclose(out) < 0) { perror(name); return 1; }
            out = NULL; lines = 0;
        }
    }
    if (ferror(in)) { perror("split: read"); return 1; }
    if (out && fclose(out) < 0) { perror("split: close"); return 1; }
    return 0;
}
