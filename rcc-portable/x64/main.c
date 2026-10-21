#include "x64.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>

static char *temporary;

static void cleanup(void)
{
    if (temporary)
        unlink(temporary);
}

int main(int argc, char **argv)
{
    const char *input = NULL, *output = NULL;
    int i, fd;
    char *path;
    FILE *file;
    x64_source_path = "rcc-x64-as";
    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-o") && i + 1 < argc)
            output = argv[++i];
        else if (!strcmp(argv[i], "--help")) {
            puts("Usage: rcc-x64-as INPUT.s -o OUTPUT.o");
            return 0;
        } else if (argv[i][0] == '-' || input)
            x64_error("usage: rcc-x64-as INPUT.s -o OUTPUT.o");
        else
            input = argv[i];
    }
    if (!input || !output || !strcmp(input, output))
        x64_error("distinct input and output files are required");
    x64_source_path = input;
    file = fopen(input, "rb");
    if (!file)
        x64_error("cannot open input");
    x64_assemble(file);
    if (fclose(file))
        x64_error("cannot close input");
    path = x64_memory(strlen(output) + 40);
    sprintf(path, "%s.tmp.%u", output, (unsigned)getpid());
    fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (fd < 0)
        x64_error("cannot create temporary output");
    temporary = path;
    atexit(cleanup);
    if (close(fd))
        x64_error("cannot close temporary output");
    file = fopen(temporary, "wb");
    if (!file)
        x64_error("cannot create output");
    x64_write_object(file);
    if (fflush(file) || ferror(file) || fclose(file))
        x64_error("cannot finish object");
    if (rename(temporary, output))
        x64_error("cannot publish object");
    free(temporary);
    temporary = NULL;
    return 0;
}
