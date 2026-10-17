/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TAPE_SIZE 30000U
#define PROGRAM_SIZE 30000U

int main(int argc, char **argv)
{
    FILE *file = NULL;
    char *program = NULL;
    unsigned *jump = NULL;
    unsigned char *tape = NULL;
    unsigned length = 0, top = PROGRAM_SIZE, open, pc, pointer = 0;
    int c, status = 1;

    if (argc == 1 || (argc == 2 && !strcmp(argv[1], "--help"))) {
        puts("usage: bf FILE\nRun a Brainfuck file; program input comes from stdin.");
        return 0;
    }
    if (argc != 2) {
        fputs("usage: bf FILE\n", stderr);
        return 1;
    }
    file = fopen(argv[1], "r");
    if (!file) { perror(argv[1]); return 1; }
    program = malloc(PROGRAM_SIZE);
    jump = malloc((unsigned long)PROGRAM_SIZE * sizeof(*jump));
    tape = calloc(TAPE_SIZE, sizeof(*tape));
    if (!program || !jump || !tape) { perror("bf"); goto done; }

    for (;;) {
        c = fgetc(file);
        if (ferror(file)) { perror(argv[1]); goto done; }
        if (feof(file)) break;
        switch (c) {
        case '>': case '<': case '+': case '-':
        case '.': case ',': case '[': case ']': break;
        default: continue;
        }
        if (length == PROGRAM_SIZE) {
            fputs("bf: program exceeds 30000 instructions\n", stderr);
            goto done;
        }
        program[length] = c;
        if (c == '[') {
            /* Unclosed brackets form a stack in the jump table itself. */
            jump[length] = top;
            top = length;
        } else if (c == ']') {
            if (top == PROGRAM_SIZE) {
                fputs("bf: unmatched ]\n", stderr);
                goto done;
            }
            open = top;
            top = jump[open];
            jump[open] = length;
            jump[length] = open;
        }
        ++length;
    }
    if (top != PROGRAM_SIZE) {
        fputs("bf: unmatched [\n", stderr);
        goto done;
    }
    fclose(file);
    file = NULL;

    for (pc = 0; pc < length; ++pc) {
        switch (program[pc]) {
        case '>': if (++pointer == TAPE_SIZE) pointer = 0; break;
        case '<': if (!pointer) pointer = TAPE_SIZE; --pointer; break;
        /* Ripple C characters are 16 bits; Brainfuck cells remain 8 bits. */
        case '+': tape[pointer] = (tape[pointer] + 1) & 255; break;
        case '-': tape[pointer] = (tape[pointer] + 255) & 255; break;
        case '.': if (putchar(tape[pointer]) == EOF) goto io_error; break;
        case ',':
            if (fflush(stdout) < 0) goto io_error;
            c = getchar();
            if (ferror(stdin)) goto io_error;
            tape[pointer] = feof(stdin) ? 0 : c & 255;
            break;
        case '[': if (!tape[pointer]) pc = jump[pc]; break;
        case ']': if (tape[pointer]) pc = jump[pc]; break;
        }
    }
    if (fflush(stdout) < 0) goto io_error;
    status = 0;
    goto done;
io_error:
    perror("bf");
done:
    if (file) fclose(file);
    free(tape);
    free(jump);
    free(program);
    return status;
}
