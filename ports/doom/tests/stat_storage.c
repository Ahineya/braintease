/* Exercise the actual lazy statistics store without file output or engine boot. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "statdump.c"

static boolean capture_enabled;

boolean M_ParmExists(char *name)
{
    return capture_enabled && strcmp(name, "-statdump") == 0;
}

void I_Error(char *format, ...)
{
    (void) format;
    puts("STAT STORAGE allocation failed");
    exit(1);
}

int main(void)
{
    wbstartstruct_t input;
    wbstartstruct_t *saved;
    int index;
    memset(&input, 0, sizeof(input));
    StatCopy(&input);
    if (captured_stats != NULL || num_captured_stats != 0) return 1;
    capture_enabled = true;
    input.last = 7;
    input.partime = 80000L;
    input.plyr[0].stime = 70000L;
    StatCopy(&input);
    if (captured_stats == NULL || num_captured_stats != 1) return 2;
    saved = captured_stats;
    for (index = 1; index < MAX_CAPTURES; ++index)
    {
        input.last = index;
        StatCopy(&input);
    }
    input.last = -1;
    StatCopy(&input);
    if (captured_stats != saved || num_captured_stats != MAX_CAPTURES) return 3;
    if (captured_stats[0].last != 7 || captured_stats[MAX_CAPTURES - 1].last != MAX_CAPTURES - 1) return 4;
    if (captured_stats[MAX_CAPTURES - 1].partime != 80000L || captured_stats[0].plyr[0].stime != 70000L) return 5;
    free(captured_stats);
    captured_stats = NULL;
    puts("STAT STORAGE OK 32 retained snapshots, lazy allocation, wide times");
    return 0;
}
