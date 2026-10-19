#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "ripple.h"
static char *pending;

static void cleanup(void)
{
    if (pending)
        unlink(pending);
}

int main(int argc, char **argv)
{
    const char *source = NULL, *destination = NULL, *unit = NULL;
    FILE *in = stdin, *out = stdout;
    struct module *module;
    int i;
    filename = "<stdin>";
    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-o") && i + 1 < argc)
            destination = argv[++i];
        else if (!strcmp(argv[i], "--unit") && i + 1 < argc)
            unit = argv[++i];
        else if (argv[i][0] == '-' || source)
            fail("usage: rcc-ripple [--unit ID] [-o ASSEMBLY] [IR]");
        else
            source = argv[i];
    }
    if (source) {
        filename = source;
        in = fopen(source, "r");
        if (!in)
            fail("cannot open IR input");
    }
    if (source && destination && !strcmp(source, destination))
        fail("output must not overwrite IR input");
    module = parse(in, unit ? unit : filename);
    if (source)
        fclose(in);
    if (destination) {
        pending = malloc(strlen(destination) + 40);
        if (!pending)
            fail("out of memory");
        sprintf(pending, "%s.tmp.%u", destination, (unsigned)getpid());
        atexit(cleanup);
        out = fopen(pending, "w");
        if (!out)
            fail("cannot create assembly output");
    }
    emit_module(module, out);
    if (destination) {
        if (fclose(out) || rename(pending, destination))
            fail("cannot publish assembly output");
        free(pending);
        pending = NULL;
    }
    return 0;
}
