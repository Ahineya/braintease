#include <stdio.h>

#define TAPE_SIZE 4096
#define PROG_SIZE 2048

static unsigned char tape[TAPE_SIZE];
static char prog[PROG_SIZE];

static void run(char *src) {
    int ptr;
    int pc;
    int nest;
    int i;
    int c;

    i = 0;
    while (i < TAPE_SIZE) {
        tape[i] = 0;
        i = i + 1;
    }

    ptr = 0;
    pc = 0;
    while (src[pc]) {
        c = (int)(unsigned char)src[pc];
        switch (c) {
        case '>':
            ptr = ptr + 1;
            if (ptr >= TAPE_SIZE) {
                ptr = 0;
            }
            break;
        case '<':
            ptr = ptr - 1;
            if (ptr < 0) {
                ptr = TAPE_SIZE - 1;
            }
            break;
        case '+':
            tape[ptr] = (unsigned char)((tape[ptr] + 1) & 255);
            break;
        case '-':
            tape[ptr] = (unsigned char)((tape[ptr] - 1) & 255);
            break;
        case '.':
            putchar((int)tape[ptr]);
            break;
        case ',':
            tape[ptr] = (unsigned char)(getchar() & 255);
            break;
        case '[':
            if (tape[ptr] == 0) {
                nest = 1;
                pc = pc + 1;
                while (nest && src[pc]) {
                    if (src[pc] == '[') {
                        nest = nest + 1;
                    } else if (src[pc] == ']') {
                        nest = nest - 1;
                    }
                    if (nest) {
                        pc = pc + 1;
                    }
                }
                if (nest) {
                    puts("unmatched [");
                    return;
                }
            }
            break;
        case ']':
            if (tape[ptr] != 0) {
                nest = 1;
                pc = pc - 1;
                while (nest && pc >= 0) {
                    if (src[pc] == ']') {
                        nest = nest + 1;
                    } else if (src[pc] == '[') {
                        nest = nest - 1;
                    }
                    if (nest) {
                        pc = pc - 1;
                    }
                }
                if (nest) {
                    puts("unmatched ]");
                    return;
                }
            }
            break;
        default:
            break;
        }
        pc = pc + 1;
    }
}

int main() {
    int n;
    int ch;

    puts("Simple Brainfuck interpreter");
    puts("Type a program and press Enter to run.");
    printf("> ");

    n = 0;
    while (n < PROG_SIZE - 1) {
        ch = getchar();
        if (ch == '\r') {
            ch = '\n';
        }
        if (ch == '\n') {
            putchar('\n');
            break;
        }
        if (ch == 8 || ch == 127) {
            if (n > 0) {
                n = n - 1;
                putchar(8);
                putchar(' ');
                putchar(8);
            }
            continue;
        }
        putchar(ch);
        prog[n] = (char)ch;
        n = n + 1;
    }
    prog[n] = 0;

    run(prog);
    putchar('\n');
    return 0;
}
