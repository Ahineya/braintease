#include "wad.h"
#include <stdio.h>
#include <mmio.h>

unsigned short cur_hi;
unsigned short cur_lo;

void print_digit(int n) {
    if (n >= 0 && n <= 9) {
        putchar('0' + n);
    } else {
        putchar('?');
    }
}

void print_uint(int n) {
    if (n >= 10000) {
        print_digit(n / 10000);
        print_digit((n / 1000) % 10);
        print_digit((n / 100) % 10);
        print_digit((n / 10) % 10);
        print_digit(n % 10);
    } else if (n >= 1000) {
        print_digit(n / 1000);
        print_digit((n / 100) % 10);
        print_digit((n / 10) % 10);
        print_digit(n % 10);
    } else if (n >= 100) {
        print_digit(n / 100);
        print_digit((n / 10) % 10);
        print_digit(n % 10);
    } else if (n >= 10) {
        print_digit(n / 10);
        print_digit(n % 10);
    } else {
        print_digit(n);
    }
}

void print_hex_digit(unsigned int n) {
    n = n & 0xF;
    if (n < 10) {
        putchar('0' + n);
    } else {
        putchar('a' + (n - 10));
    }
}

void print_hex16(unsigned int n) {
    print_hex_digit(n >> 12);
    print_hex_digit(n >> 8);
    print_hex_digit(n >> 4);
    print_hex_digit(n);
}

void print_hex32(unsigned int hi, unsigned int lo) {
    print_hex16(hi);
    print_hex16(lo);
}

void br(void) {
    putchar('\n');
}

void seek_to(unsigned short hi, unsigned short lo) {
    cur_hi = hi;
    cur_lo = lo;
}

void skip(unsigned short n) {
    unsigned short old = cur_lo;
    unsigned short next = old + n;
    cur_lo = next;
    if (next < old) {
        cur_hi = cur_hi + 1;
    }
}

unsigned short read8(void) {
    unsigned short b = storage_read_at(cur_hi, cur_lo) & 0xFF;
    skip(1);
    return b;
}

unsigned short read16(void) {
    unsigned short lo_b = read8();
    unsigned short hi_b = read8();
    return lo_b | (hi_b << 8);
}

int names_equal(char *a, char *b) {
    int i;
    for (i = 0; i < 8; i++) {
        if (a[i] != b[i]) {
            return 0;
        }
    }
    return 1;
}
