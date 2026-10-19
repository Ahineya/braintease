/* Bounded ANSI/VT cursor-key decoding; see upstream/dat/license. */
#include <stdio.h>
#include <sys/select.h>
#include <time.h>
#include <unistd.h>
#include "keys.h"

/* Incomplete sequences and Meta-key combinations retain their literal bytes. */
static int pending[32], used, at;

static unsigned long milliseconds(void)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) < 0) return 0;
    return (unsigned long) now.tv_sec * 1000UL + now.tv_nsec / 1000000L;
}

static int input(int delay)
{
    fd_set reads;
    struct timeval timeout;
    int fd = fileno(stdin);
    if (at < used) return pending[at++];
    /* getchar may already have read the rest of an escape sequence. */
    if (stdin->pushed != EOF || stdin->pos < stdin->len) return getchar();
    if (delay < 0) return getchar();
    if (fd < 0 || fd >= FD_SETSIZE) return EOF;
    FD_ZERO(&reads);
    FD_SET(fd, &reads);
    timeout.tv_sec = 0;
    timeout.tv_usec = (unsigned long) delay * 1000UL;
    if (select(fd + 1, &reads, NULL, NULL, &timeout) <= 0) return EOF;
    return getchar();
}

int nh_terminal_getchar(const char *directions)
{
    int c = input(-1), sequence[32], n = 0, i, direction = -1;
    unsigned long start, elapsed;
    if (c != 27) return c;
    start = milliseconds();
    while (n < 32) {
        elapsed = milliseconds() - start;
        c = input(elapsed >= 150 ? 0 : 150 - (int) elapsed);
        if (c == EOF) break;
        sequence[n++] = c;
        if (n == 1 && c != '[' && c != 'O') break;
        if (n >= 2 && c >= '@' && c <= '~') break;
    }
    /* Only unmodified arrows are movement: modified arrows must not turn
     * into unintended running or other game commands. */
    if (n == 2 && (sequence[0] == '[' || sequence[0] == 'O')) {
        switch (sequence[1]) {
        case 'A': direction = 0; break;
        case 'B': direction = 1; break;
        case 'C': direction = 2; break;
        case 'D': direction = 3; break;
        }
    }
    if (direction >= 0) {
        /* Menus/prompts get a single cancel, never a selection letter. */
        static const int indices[] = { 2, 6, 4, 0 };
        if (!directions) return 27;
        return directions[indices[direction]];
    }
    /* A terminal control sequence is not game text; consume unsupported
     * CSI/SS3 keys as one cancel. Preserve incomplete sequences verbatim. */
    if (n >= 2 && (sequence[0] == '[' || sequence[0] == 'O')
        && sequence[n - 1] >= '@' && sequence[n - 1] <= '~') return 27;
    /* input() drained any previous pending bytes before the descriptor. */
    used = n; at = 0;
    for (i = 0; i < n; ++i) pending[i] = sequence[i];
    return 27;
}
