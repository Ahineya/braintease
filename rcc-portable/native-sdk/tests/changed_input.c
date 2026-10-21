/* The linker must reject a file replaced after input cataloguing. */
#include "../../x64-link/link.h"
#include <stdlib.h>

int main(int argc, char **argv)
{
    FILE *f;
    if (argc != 4)
        return 2;
    ld_metadata(argv[1]);
    ld_input(argv[2]);
    f = fopen(argv[2], "r+b");
    if (!f || fseek(f, 64, SEEK_SET) || putc(42, f) == EOF || fclose(f))
        return 2;
    ld_modules(1);
    ld_link();
    /* Reaching output publication would be a bug. */
    f = fopen(argv[3], "wb");
    if (!f)
        return 2;
    ld_write(f);
    return fclose(f) != 0;
}
