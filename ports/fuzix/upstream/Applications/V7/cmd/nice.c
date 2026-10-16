/* From UNIX V7 source code: see /COPYRIGHT or www.tuhs.org for details. */

/* nice */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>

/* Ripple adaptation: validate increments and retain every command argument. */
#include <limits.h>
static int increment(const char *s) {
    char *end;
    long value;
    errno = 0; value = strtol(s, &end, 10);
    if (errno || end == s || *end || value < INT_MIN || value > INT_MAX) {
        fputs("nice: invalid increment\n", stderr); exit(125);
    }
    /* Every valid current nice value is -20..19. Avoid overflowing the
       kernel's 16-bit addition while retaining its saturation behavior. */
    return value > 40 ? 40 : value < -40 ? -40 : (int)value;
}
int main(int argc, char *argv[]) {
    int adjustment = 10, first = 1;
    if (argc > first && !strcmp(argv[first], "--")) first++;
    else if (argc > first && !strncmp(argv[first], "-n", 2)) {
        if (argv[first][2]) adjustment = increment(argv[first++] + 2);
        else {
            if (argc <= first + 1) { fputs("nice: missing increment\n", stderr); return 125; }
            adjustment = increment(argv[first + 1]); first += 2;
        }
    } else if (argc > first && argv[first][0] == '-') adjustment = increment(argv[first++] + 1);
    if (argc <= first) { fputs("usage: nice [-n increment] command [args]\n", stderr); return 125; }
    errno = 0;
    if (nice(adjustment) == -1 && errno) { perror("nice"); return 125; }
    execvp(argv[first], &argv[first]);
    first = errno == ENOENT ? 127 : 126;
    perror("nice"); return first;
}
