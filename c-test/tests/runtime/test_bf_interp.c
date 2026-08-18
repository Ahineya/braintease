#include <stdio.h>

#define TAPE_SIZE 4096

static unsigned char tape[TAPE_SIZE];

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
            }
            break;
        default:
            break;
        }
        pc = pc + 1;
    }
}

int main() {
    run("++++++++++++++++++++++++++++++++++++++++++++++++.");
    run("++++++++[>++++[>++>+++>+++>+<<<<-]>+>+>->>+[<]<-]>>.>---.+++++++..+++.>>.<-.<.+++.------.--------.>>+.>++.");
    return 0;
}
