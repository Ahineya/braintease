#include "link.h"
#include <string.h>

void ld_sdk(const char *directory, const char *output)
{
    unsigned arity = ld_program_arity();
    char *startup = ld_memory(strlen(directory) + 32);
    char *library = ld_memory(strlen(directory) + 32);
    sprintf(startup, "%s/crt0-%u.o", directory, arity);
    sprintf(library, "%s/libc.a", directory);
    if (!strcmp(output, startup) || !strcmp(output, library))
        ld_error("output must not overwrite SDK input");
    ld_input(startup);
    ld_input(library);
    ld_entry_symbol = "__rcc_libc_entry";
}
