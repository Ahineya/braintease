#ifndef ROS_UTIL_C
#define ROS_UTIL_C

#include "util.h"
#include <stdio.h>
#include <ctype.h>

int str_eq_i(char *a, char *b) {
    int i;
    int ca;
    int cb;

    i = 0;
    while (1) {
        ca = toupper((int)(unsigned char)a[i]);
        cb = toupper((int)(unsigned char)b[i]);
        if (ca != cb) {
            return 0;
        }
        if (ca == 0) {
            return 1;
        }
        i = i + 1;
    }
}

int is_path_sep(int c) {
    return c == '/' || c == '\\';
}

int read_line(char *buf, int max) {
    int i;
    int c;

    i = 0;
    while (i < max - 1) {
        c = getchar();
        if (c == EOF) {
            break;
        }
        if (c == '\r') {
            c = '\n';
        }
        if (c == '\n') {
            putchar('\n');
            break;
        }
        if (c == 8 || c == 127) {
            if (i > 0) {
                i = i - 1;
                putchar(8);
                putchar(' ');
                putchar(8);
            }
            continue;
        }
        putchar(c);
        buf[i] = (char)c;
        i = i + 1;
    }
    buf[i] = 0;
    return i;
}

#endif
