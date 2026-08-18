#include <stdio.h>
#include <stdint.h>
#include "../../examples/rOS/disk.c"
#include "../../examples/rOS/fat16.c"

int main() {
    char o[11];
    char n[13];
    uint8_t raw[11];
    int i;

    fat16_canon83("readme.txt", o);
    if (o[0] == 'R' && o[1] == 'E' && o[5] == 'E' && o[6] == ' ' && o[8] == 'T' && o[10] == 'T') {
        putchar('Y');
    } else {
        putchar('N');
    }

    fat16_canon83("DOCS", o);
    if (o[0] == 'D' && o[3] == 'S' && o[4] == ' ' && o[8] == ' ') {
        putchar('Y');
    } else {
        putchar('N');
    }

    fat16_canon83(".", o);
    if (o[0] == '.' && o[1] == ' ') {
        putchar('Y');
    } else {
        putchar('N');
    }

    fat16_canon83("..", o);
    if (o[0] == '.' && o[1] == '.' && o[2] == ' ') {
        putchar('Y');
    } else {
        putchar('N');
    }

    i = 0;
    while (i < 11) {
        raw[i] = (uint8_t)' ';
        i = i + 1;
    }
    raw[0] = 'R';
    raw[1] = 'E';
    raw[2] = 'A';
    raw[3] = 'D';
    raw[4] = 'M';
    raw[5] = 'E';
    raw[8] = 'T';
    raw[9] = 'X';
    raw[10] = 'T';
    fat16_format83(raw, n);
    if (n[0] == 'R' && n[6] == '.' && n[7] == 'T' && n[8] == 'X' && n[9] == 'T' && n[10] == 0) {
        putchar('Y');
    } else {
        putchar('N');
    }

    fat16_canon83("readme.txt", o);
    i = 0;
    while (i < 11 && (char)raw[i] == o[i]) {
        i = i + 1;
    }
    if (i == 11) {
        putchar('Y');
    } else {
        putchar('N');
    }

    putchar('\n');
    return 0;
}
