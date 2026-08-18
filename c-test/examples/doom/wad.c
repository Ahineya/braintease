#include "wad.h"
#include <mmio.h>

unsigned short cur_hi;
unsigned short cur_lo;

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
    // STORE_ADDR is a word index: file bytes 2n,2n+1 live in word n (LE).
    unsigned short word = storage_read_at(cur_hi, cur_lo >> 1);
    unsigned short b;
    if (cur_lo & 1) {
        b = (word >> 8) & 0xFF;
    } else {
        b = word & 0xFF;
    }
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
