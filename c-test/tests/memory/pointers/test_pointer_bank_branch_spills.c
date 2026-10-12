#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int argument_count;
static char **arguments;

static int scan(char *needle, int trailing)
{
    int index;
    for (index = 1; index < argument_count - trailing; ++index) {
        if (!strcmp(needle, arguments[index])) return index;
    }
    return 0;
}

static void poison_stack(void)
{
    unsigned short stale[512];
    int i;
    for (i = 0; i < 512; ++i) stale[i] = 0x4455;
}

int main(void)
{
    static char *global_args[] = {"program", "-iwad", "doom.wad", "-warp", "1", "1"};
    char local_needle[] = "-warp";
    char *local_args[6];
    char *heap_needle = malloc(6);
    char **heap_args = malloc(6 * sizeof(*heap_args));
    int i;
    argument_count = 6;
    arguments = global_args;
    poison_stack();
    putchar(scan("-iwad", 1) == 1 ? 'Y' : 'N');
    poison_stack();
    putchar(scan("missing", 0) == 0 && scan("1", 1) == 4 ? 'Y' : 'N');
    for (i = 0; i < 6; ++i) local_args[i] = global_args[i];
    arguments = local_args;
    poison_stack();
    putchar(scan(local_needle, 2) == 3 ? 'Y' : 'N');
    strcpy(heap_needle, "-warp");
    for (i = 0; i < 6; ++i) heap_args[i] = global_args[i];
    arguments = heap_args;
    poison_stack();
    putchar(scan(heap_needle, 2) == 3 ? 'Y' : 'N');
    free(heap_args);
    free(heap_needle);
    putchar('\n');
    return 0;
}
