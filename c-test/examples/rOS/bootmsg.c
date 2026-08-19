#include <stdio.h>
#include "bootmsg.h"

void boot_sgr(int n) {
    putchar(27);
    putchar('[');
    if (n >= 100) {
        putchar('0' + (n / 100));
        n = n - (n / 100) * 100;
        putchar('0' + (n / 10));
        putchar('0' + (n % 10));
    } else if (n >= 10) {
        putchar('0' + (n / 10));
        putchar('0' + (n % 10));
    } else {
        putchar('0' + n);
    }
    putchar('m');
}

static void boot_cstr(char *s) {
    int i;

    i = 0;
    while (s[i]) {
        putchar(s[i]);
        i = i + 1;
    }
}

void boot_stage(char *name) {
    putchar('\n');
    boot_sgr(BOOT_BOLD);
    boot_sgr(BOOT_CYAN);
    boot_cstr(name);
    boot_sgr(BOOT_RESET);
    putchar('\n');
}

static void boot_ok(void) {
    putchar('[');
    putchar('O');
    putchar('K');
    putchar(']');
}

void boot_item(char *label, char *value) {
    int i;

    boot_sgr(BOOT_DIM);
    putchar(' ');
    putchar(' ');
    i = 0;
    while (label[i] && i < 12) {
        putchar(label[i]);
        i = i + 1;
    }
    while (i < 12) {
        putchar('.');
        i = i + 1;
    }
    putchar(' ');
    boot_sgr(BOOT_RESET);
    boot_cstr(value);
    putchar(' ');
    boot_sgr(BOOT_GREEN);
    boot_ok();
    boot_sgr(BOOT_RESET);
    putchar('\n');
}

void boot_load(char *name) {
    int i;

    boot_sgr(BOOT_DIM);
    putchar(' ');
    putchar(' ');
    i = 0;
    while (name[i] && i < 12) {
        putchar(name[i]);
        i = i + 1;
    }
    while (i < 12) {
        putchar('.');
        i = i + 1;
    }
    putchar(' ');
    boot_sgr(BOOT_YEL);
    putchar('l');
    putchar('o');
    putchar('a');
    putchar('d');
    putchar('i');
    putchar('n');
    putchar('g');
    boot_sgr(BOOT_RESET);
    putchar('\n');
}

void boot_fail(char *msg) {
    boot_sgr(BOOT_RED);
    boot_sgr(BOOT_BOLD);
    putchar(' ');
    putchar(' ');
    putchar('E');
    putchar('R');
    putchar('R');
    putchar('O');
    putchar('R');
    putchar(':');
    putchar(' ');
    boot_sgr(BOOT_RESET);
    boot_sgr(BOOT_RED);
    boot_cstr(msg);
    putchar('\n');
    boot_sgr(BOOT_RESET);
}

void boot_banner(char *title) {
    putchar('\n');
    boot_sgr(BOOT_BOLD);
    boot_sgr(BOOT_BLUE);
    putchar(' ');
    putchar(' ');
    boot_cstr(title);
    boot_sgr(BOOT_RESET);
    putchar('\n');
}

void boot_palette(void) {
    int i;

    putchar(' ');
    putchar(' ');
    i = 0;
    while (i < 8) {
        boot_sgr(40 + i);
        putchar(' ');
        putchar(' ');
        i = i + 1;
    }
    boot_sgr(BOOT_RESET);
    putchar(' ');
    i = 0;
    while (i < 8) {
        boot_sgr(100 + i);
        putchar(' ');
        putchar(' ');
        i = i + 1;
    }
    boot_sgr(BOOT_RESET);
    putchar('\n');
}
