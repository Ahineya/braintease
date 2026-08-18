#ifndef ROS_DISK_H
#define ROS_DISK_H

#include <stdint.h>

/* Byte-addressed view of RVM block storage.
 *
 * File offset 0xHHHHLLLL maps to storage block 0xHHHH, byte 0xLLLL.
 * Storage words are little-endian, so byte 0 is the low 8 bits of word 0.
 */

uint8_t disk_read8(uint32_t off);
uint16_t disk_read16(uint32_t off);
uint32_t disk_read32(uint32_t off);
void disk_read(uint32_t off, uint8_t *buf, uint16_t n);

void disk_write8(uint32_t off, uint8_t v);
void disk_write16(uint32_t off, uint16_t v);
void disk_write32(uint32_t off, uint32_t v);

#endif
