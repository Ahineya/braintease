#include "wad.h"
#include <mmio.h>

static uint32_t cur;

void wad_seek(uint32_t offset) {
    cur = offset;
}

void wad_skip(uint32_t n) {
    cur = cur + n;
}

uint16_t wad_read8(void) {
    uint16_t block = (uint16_t)(cur >> 16);
    uint16_t byte_addr = (uint16_t)cur;
    uint16_t word = storage_read_at(block, byte_addr >> 1);
    uint16_t b;

    if (byte_addr & 1) {
        b = (word >> 8) & 0xFF;
    } else {
        b = word & 0xFF;
    }

    cur = cur + 1;
    return b;
}

uint16_t wad_read16(void) {
    uint16_t lo = wad_read8();
    uint16_t hi = wad_read8();
    return lo | (uint16_t)(hi << 8);
}

uint32_t wad_read32(void) {
    uint32_t lo = wad_read16();
    uint32_t hi = wad_read16();
    return lo | (hi << 16);
}

void wad_read_name(char *name) {
    int i;
    for (i = 0; i < 8; i++) {
        name[i] = (char)wad_read8();
    }
}

int wad_name_eq(char *name, char *want) {
    int i;
    for (i = 0; i < 8; i++) {
        if (name[i] != want[i]) {
            return 0;
        }
    }
    return 1;
}
