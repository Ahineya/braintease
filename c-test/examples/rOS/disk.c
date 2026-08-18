#ifndef ROS_DISK_C
#define ROS_DISK_C

#include "disk.h"
#include <mmio.h>

static uint16_t disk_word_at(uint32_t off) {
    uint16_t block;
    uint16_t byte_addr;

    block = (uint16_t)(off >> 16);
    byte_addr = (uint16_t)off;
    return storage_read_at(block, byte_addr >> 1);
}

uint8_t disk_read8(uint32_t off) {
    uint16_t word;
    uint16_t byte_addr;

    word = disk_word_at(off);
    byte_addr = (uint16_t)off;
    if (byte_addr & 1) {
        return (uint8_t)((word >> 8) & 0xFF);
    }
    return (uint8_t)(word & 0xFF);
}

uint16_t disk_read16(uint32_t off) {
    uint16_t lo;
    uint16_t hi;

    lo = disk_read8(off);
    hi = disk_read8(off + 1);
    return lo | (uint16_t)(hi << 8);
}

uint32_t disk_read32(uint32_t off) {
    uint32_t lo;
    uint32_t hi;

    lo = disk_read16(off);
    hi = disk_read16(off + 2);
    return lo | (hi << 16);
}

void disk_read(uint32_t off, uint8_t *buf, uint16_t n) {
    uint16_t i;

    i = 0;
    while (i < n) {
        buf[i] = disk_read8(off + i);
        i = i + 1;
    }
}

void disk_write8(uint32_t off, uint8_t v) {
    uint16_t block;
    uint16_t byte_addr;
    uint16_t word_addr;
    uint16_t word;

    block = (uint16_t)(off >> 16);
    byte_addr = (uint16_t)off;
    word_addr = byte_addr >> 1;
    word = storage_read_at(block, word_addr);
    if (byte_addr & 1) {
        word = (uint16_t)((word & 0x00FF) | ((uint16_t)v << 8));
    } else {
        word = (uint16_t)((word & 0xFF00) | v);
    }
    storage_write_at(block, word_addr, word);
}

void disk_write16(uint32_t off, uint16_t v) {
    disk_write8(off, (uint8_t)(v & 0xFF));
    disk_write8(off + 1, (uint8_t)((v >> 8) & 0xFF));
}

void disk_write32(uint32_t off, uint32_t v) {
    disk_write16(off, (uint16_t)(v & 0xFFFF));
    disk_write16(off + 2, (uint16_t)((v >> 16) & 0xFFFF));
}

#endif
