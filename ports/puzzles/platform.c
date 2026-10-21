/* SPDX-License-Identifier: MIT */
/* Platform-independent support also used by the engine regression tests. */
#include <stdarg.h>
#include <fcntl.h>
#include <unistd.h>
#include <math.h>
#include "puzzles.h"

char ver[] = "20260923.616da16 / Riplix";

/* The guest SDK exports double libm; Mines also requests the float entry. */
float sqrtf(float x) { return (float)sqrt((double)x); }

void fatal(const char *fmt, ...)
{
    va_list ap;
    fputs("puzzles: ", stderr);
    va_start(ap, fmt); vfprintf(stderr, fmt, ap); va_end(ap);
    fputc('\n', stderr);
    exit(1);
}

void get_random_seed(void **seed, int *size)
{
    unsigned char *bytes = snewn(32, unsigned char);
    int fd = open("/dev/urandom", O_RDONLY), used = 0, n, i;
    if (fd < 0) fatal("cannot open /dev/urandom");
    while (used < 32) {
        n = read(fd, bytes + used, 32 - used);
        if (n <= 0) { close(fd); fatal("cannot read random seed"); }
        used += n;
    }
    close(fd);
    /* random.c consumes octets, even when a C char occupies 16 bits. */
    for (i = 0; i < 32; i++) bytes[i] &= 255;
    *seed = bytes; *size = 32;
}

void frontend_default_colour(frontend *fe, float *out)
{
    (void)fe;
    out[0] = 0.82F; out[1] = 0.84F; out[2] = 0.85F;
}

/* This frontend does not advertise printing. Never silently discard it. */
void document_add_puzzle(document *d, const game *g, game_params *p,
                         game_ui *u, game_state *s, game_state *solution)
{
    (void)d; (void)g; (void)p; (void)u; (void)s; (void)solution;
    fatal("printing is not supported by the Nano-X frontend");
}
