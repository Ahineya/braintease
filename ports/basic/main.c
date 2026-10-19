/* SPDX-License-Identifier: MIT */
#include "basic.h"

Program basic;
Instruction *location;
const char *source_path;
unsigned long column = 1;
uint32_t random_state = 1;
int check_only;

void fail(const char *message) {
    fprintf(stderr, "%s:%lu:%lu: %s\n", source_path ? source_path : "basic",
            location ? location->line : 1UL, column, message);
    exit(1);
}
void *allocate(size_t size) {
    void *p = calloc(1, size ? size : 1);
    if (!p) fail("out of memory");
    return p;
}
char *copy_range(const char *text, unsigned n) {
    char *p;
    if (n > BASIC_STRING) fail("string exceeds 4096 characters");
    p = allocate((size_t)n + 1);
    memcpy(p, text, n);
    return p;
}
char *copy_text(const char *text) { return copy_range(text, (unsigned)strlen(text)); }

int main(int argc, char **argv) {
    FILE *input;
    int i;
    for (i=1; i<argc; ++i) {
        if (!strcmp(argv[i], "--check")) check_only=1;
        else if (!strcmp(argv[i], "--graphics=auto") || !strcmp(argv[i], "--graphics=off")) {}
        else if (!strcmp(argv[i], "--graphics=on")) {
            fputs("basic: this build supports terminal programs; Nano-X graphics are not implemented yet\n", stderr);
            return 2;
        } else if (argv[i][0]=='-' || source_path) {
            fputs("usage: basic [--check] [--graphics=auto|off|on] FILE.bas\n", stderr);
            return 2;
        } else source_path=argv[i];
    }
    if (!source_path) {
        fputs("usage: basic [--check] [--graphics=auto|off|on] FILE.bas\n", stderr);
        return 2;
    }
    input=fopen(source_path, "r");
    if (!input) { perror(source_path); return 1; }
    load_program(input);
    if (fclose(input)) fail("cannot close source file");
    {
        int requested_check=check_only;
        check_only=1; run_program();
        check_only=requested_check;
        if (check_only) printf("%s: syntax OK\n", source_path);
        else run_program();
    }
    free_program();
    return 0;
}
