/* Portable C16 preprocessor and RCC IR frontend driver. */
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "util.h"
#include "cc.h"

void rcc_finish(void);
void ppstart(const char *);

int main(int argc, char **argv)
{
    const char *input = NULL, *output = NULL, *target = "ripple";
    int i;
    bool preprocess = false, preprocessed = false;
    argv0 = argv[0];
    tokeninit();
    ppdefine("__STDC__=1");
    ppdefine("__STDC_VERSION__=201112L");
    ppdefine("__STDC_HOSTED__=0");
    ppdefine("__RCC_PORTABLE__=1");
    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--target") && i + 1 < argc)
            target = argv[++i];
        else if (!strcmp(argv[i], "-o") && i + 1 < argc)
            output = argv[++i];
        else if (!strcmp(argv[i], "-E"))
            preprocess = true;
        else if (!strcmp(argv[i], "--preprocessed"))
            preprocessed = true;
        else if (argv[i][0] == '-' &&
                 (argv[i][1] == 'I' || argv[i][1] == 'D' || argv[i][1] == 'U')) {
            char option = argv[i][1];
            const char *value = argv[i] + 2;
            if (!*value) {
                if (i + 1 == argc)
                    fatal("missing option argument");
                value = argv[++i];
            }
            if (option == 'I')
                ppinclude(value);
            else if (option == 'D') {
                const char *eq = strchr(value, '=');
                char *name;
                if (eq) {
                    name = malloc((size_t)(eq - value) + 1);
                    memcpy(name, value, (size_t)(eq - value));
                    name[eq - value] = 0;
                } else {
                    name = malloc(strlen(value) + 1);
                    strcpy(name, value);
                }
                ppundef(name);
                free(name);
                ppdefine(value);
            } else
                ppundef(value);
        } else if (argv[i][0] == '-' || input)
            fatal("usage: rcc-c [-E] [-I DIR] [-D MACRO[=VALUE]] [-U MACRO] [--target "
                  "ripple|native] [-o OUTPUT] [C-SOURCE]");
        else
            input = argv[i];
    }
    if (strcmp(target, "native") && strcmp(target, "ripple"))
        fatal("unknown backend target '%s'", target);
    targinit(NULL);
    if (output && !freopen(output, "w", stdout))
        fatal("cannot open output:");
    if (input) {
        scanfrom(input, NULL);
        scanopen();
    } else
        scanfrom("<stdin>", stdin);
    (void)preprocessed; /* .i input remains accepted by the same scanner. */
    ppstart(input ? input : "<stdin>");
    if (preprocess)
        ppflags = (enum ppflags)2;
    ppinit();
    if (preprocess) {
        ppoutput(stdout);
        if (fflush(stdout) || ferror(stdout))
            fatal("cannot write preprocessed output:");
        return 0;
    }
    scopeinit();
    puts("module portable {");
    if (!strcmp(target, "native"))
        puts("  target native");
    while (tok.kind != TEOF)
        if (!decl(&filescope, NULL))
            error(&tok.loc, "expected a declaration");
    emittentativedefns();
    rcc_finish();
    puts("}");
    if (fflush(stdout) || ferror(stdout))
        fatal("cannot write IR:");
    return 0;
}
