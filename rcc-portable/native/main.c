#include "native.h"
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
static char *pending;

static void cleanup(void)
{
    if (pending)
        unlink(pending);
}

int main(int argc, char **argv)
{
    const char *source = NULL, *destination = NULL;
    struct module *module;
    struct vec inputs = {0}, kernel_inputs = {0};
    FILE *in, *out;
    char *path;
    int n, fd, relocatable = 0, no_start = 0;
    filename = "rcc-qbe";
    for (n = 1; n < argc; n++) {
        if (!strcmp(argv[n], "-o") && n + 1 < argc)
            destination = argv[++n];
        else if (!strcmp(argv[n], "--relocatable"))
            relocatable = 1;
        else if (!strcmp(argv[n], "--no-start"))
            no_start = 1;
        else if (!strcmp(argv[n], "--kernel") && n + 1 < argc)
            *(const char **)append(&kernel_inputs, sizeof(char *)) = argv[++n];
        else if (!strcmp(argv[n], "--help")) {
            puts("Usage: rcc-qbe [--relocatable] [--no-start] INPUT.ir -o OUTPUT.ssa\n"
                 "       rcc-qbe --kernel KERNEL.ir... [APP.ir...] -o KERNEL.ssa");
            return 0;
        } else if (argv[n][0] == '-')
            fail("usage: rcc-qbe INPUT.ir -o OUTPUT.ssa");
        else
            *(const char **)append(&inputs, sizeof(char *)) = argv[n];
    }
    if (!destination || (!kernel_inputs.count && inputs.count != 1) ||
        (kernel_inputs.count && (relocatable || no_start)))
        fail("distinct input and output paths are required");
    if (kernel_inputs.count) {
        size_t i;
        module = allocate(sizeof *module);
        for (i = 0; i < kernel_inputs.count + inputs.count; i++) {
            int shared = i < kernel_inputs.count;
            source = shared ? ((const char **)kernel_inputs.data)[i]
                            : ((const char **)inputs.data)[i - kernel_inputs.count];
            if (!strcmp(source, destination))
                fail("output must not overwrite input");
            filename = source;
            line_number = 1;
            in = fopen(source, "r");
            if (!in)
                fail("cannot open kernel IR input");
            native_kernel_merge(module, parse_target(in, source, "native"), (unsigned long)i,
                                shared);
            if (fclose(in))
                fail("cannot close kernel IR input");
        }
        goto output;
    }
    source = ((const char **)inputs.data)[0];
    if (!strcmp(source, destination))
        fail("output must not overwrite input");
    filename = source;
    in = fopen(source, "r");
    if (!in)
        fail("cannot open IR input");
    module = parse_target(in, source, "native");
    if (fclose(in))
        fail("cannot close input");
output:
    path = malloc(strlen(destination) + 40);
    if (!path)
        fail("out of memory");
    sprintf(path, "%s.tmp.%u", destination, (unsigned)getpid());
    fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (fd < 0)
        fail("cannot create temporary output");
    pending = path;
    atexit(cleanup);
    if (close(fd))
        fail("cannot close temporary output");
    out = fopen(path, "w");
    if (!out)
        fail("cannot open output");
    if (kernel_inputs.count)
        native_emit_kernel(module, out);
    else if (no_start)
        native_emit_library(module, out);
    else if (relocatable)
        native_emit_relocatable(module, out);
    else
        native_emit(module, out);
    if (fflush(out) || ferror(out) || fclose(out))
        fail("cannot finish QBE output");
    if (rename(path, destination))
        fail("cannot publish QBE output");
    pending = NULL;
    free(path);
    return 0;
}
