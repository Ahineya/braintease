#include <string.h>
#include "tools.h"

int main(int argc, char **argv)
{
    const char *name = strrchr(argv[0], '/');
    name = name ? name + 1 : argv[0];
    if (strstr(name, "ld"))
        return linker(argc, argv);
    if (argc > 1 && !strcmp(argv[1], "link"))
        return linker(argc - 1, argv + 1);
    if (argc > 1 && !strcmp(argv[1], "assemble"))
        return assembler(argc - 1, argv + 1);
    return assembler(argc, argv);
}
