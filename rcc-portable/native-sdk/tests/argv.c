#include <stdio.h>
#include <stdlib.h>
#include <string.h>
int main(int argc, char **argv)
{
    if (argc!=3 || strcmp(argv[1],"hello") || strcmp(argv[2],"x y") || argv[3]) return 1;
    if (!getenv("PATH")) return 2;
    puts("SDK_ARGV_OK");
    /* Preserve main's C99 implicit zero return with separate libc startup. */
}
