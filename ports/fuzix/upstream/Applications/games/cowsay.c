/*
A C version of "cowsay"

Copyright (C) 2012 Federico Pomar & Federico Bond
https://github.com/federicobond/fedux

Lightly modified for Fuzix by Brett Gordon


*/

/* Ripple adaptation: bounded stdin/argv input, wrapping and checked options. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

typedef struct {
    const char *opt;
    const char *eyes;
    const char tongue;
} design_t;

static const design_t designs[] = {
    { "-b", "==", ' ' },
    { "-d", "XX", ' ' },
    { "-g", "$$", ' ' },
    { "-p", "@@", ' ' },
    { "-s", "**", 'U' },
    { "-t", "--", ' ' },
    { "-w", "OO", ' ' },
    { "-y", "..", ' ' },
    { NULL, "\0\0", 0 }
};

static const design_t defdesign =  { NULL, "oo", ' ' };

#define MAX_MESSAGE 4096
static char message[MAX_MESSAGE + 1];
static unsigned used;

static int append(int c) {
    if (used == MAX_MESSAGE) return -1;
    message[used++] = c; message[used] = 0;
    return 0;
}
/* Return a slice without allocating a second copy of the message. */
static int nextline(unsigned *position, unsigned width, unsigned *start, unsigned *length) {
    unsigned p = *position, lastspace = 0, end;
    if (p >= used) return 0;
    *start = p;
    while (p < used && message[p] != '\n' && p - *start < width) {
        if (message[p] == ' ') lastspace = p;
        p++;
    }
    end = p;
    if (p < used && message[p] != '\n' && lastspace > *start) {
        end = lastspace; p = lastspace + 1;
        while (p < used && message[p] == ' ') p++;
    } else if (p < used && message[p] == '\n') p++;
    else if (p < used && message[p] == ' ') while (p < used && message[p] == ' ') p++;
    *length = end - *start; *position = p;
    return 1;
}
static void bubble(unsigned width) {
    unsigned p = 0, start, length, maximum = 0, rows = 0, row = 0, j;
    while (nextline(&p, width, &start, &length)) {
        if (length > maximum) maximum = length;
        rows++;
    }
    putchar(' ');
    for (j = 0; j < maximum + 2; j++) putchar('_');
    putchar('\n');
    p = 0;
    while (nextline(&p, width, &start, &length)) {
        putchar(rows == 1 ? '<' : row == 0 ? '/' : row == rows - 1 ? '\\' : '|');
        putchar(' ');
        fwrite(message + start, 1, length, stdout);
        for (j = length; j < maximum; j++) putchar(' ');
        putchar(' ');
        putchar(rows == 1 ? '>' : row == 0 ? '\\' : row == rows - 1 ? '/' : '|');
        putchar('\n'); row++;
    }
    putchar(' ');
    for (j = 0; j < maximum + 2; j++) putchar('-');
    putchar('\n');
}

void
cowsay_print_cow(const design_t *design)
{
    printf("        \\   ^__^\n"
           "         \\  (%s)\\_______\n"
           "            (__)\\       )\\/\\\n"
           "             %c  ||----w |\n"
           "                ||     ||\n",
           design->eyes, design->tongue);
}

int main(int argc, char **argv) {
    const design_t *current = &defdesign;
    unsigned width = 40;
    int arg = 1, j, c;
    while (arg < argc && argv[arg][0] == '-') {
        if (!strcmp(argv[arg], "--")) { arg++; break; }
        if (!strcmp(argv[arg], "--version")) {
            puts("cowsay 1.0 (Fedux/FUZIX)"); return 0;
        }
        if (!strcmp(argv[arg], "--help") || !strcmp(argv[arg], "-h")) {
            puts("Usage: cowsay [-b|-d|-g|-p|-s|-t|-w|-y] [-W width] [--] [message ...]\n"
                 "With no message, read standard input. Default wrap width: 40.\n"
                 "Designs: borg, dead, greedy, paranoid, stoned, tired, wired, youthful.");
            return 0;
        }
        if (!strcmp(argv[arg], "-W")) {
            char *end; long n;
            if (++arg == argc) { fputs("cowsay: -W needs a width\n", stderr); return 2; }
            n = strtol(argv[arg], &end, 10);
            if (!*argv[arg] || *end || n < 1 || n > 200) {
                fputs("cowsay: width must be 1..200\n", stderr); return 2;
            }
            width = n; arg++; continue;
        }
        for (j = 0; designs[j].opt; j++) if (!strcmp(argv[arg], designs[j].opt)) break;
        if (!designs[j].opt) { fprintf(stderr, "cowsay: unknown option %s\n", argv[arg]); return 2; }
        current = designs + j; arg++;
    }
    if (arg < argc) {
        while (arg < argc) {
            const char *p = argv[arg++];
            while (*p) if (append(*p++)) goto too_long;
            if (arg < argc && append(' ')) goto too_long;
        }
    } else {
        while ((c = getchar()) != EOF) {
            if (c == '\r') continue;
            if (c == '\t') c = ' ';
            if (append(c)) goto too_long;
        }
        if (ferror(stdin)) { perror("cowsay"); return 1; }
    }
    /* Keep terminal control codes out of the bubble. */
    for (j = 0; j < used; j++)
        if (message[j] != '\n' && (message[j] < 32 || message[j] >= 127)) message[j] = '?';
    while (used && message[used - 1] == '\n') message[--used] = 0;
    if (!used) { strcpy(message, "Nothing to say."); used = strlen(message); }
    bubble(width); cowsay_print_cow(current);
    return fflush(stdout) == EOF ? 1 : 0;
too_long:
    fprintf(stderr, "cowsay: message exceeds %d characters\n", MAX_MESSAGE);
    return 1;
}
