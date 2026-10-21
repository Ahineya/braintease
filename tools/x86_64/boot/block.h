#ifndef RIPLIX_BLOCK_H
#define RIPLIX_BLOCK_H
#include "kernel.h"
/* Synchronous native-byte interface. No guest pointers or callbacks. */
uint64_t block_init(void);
int block_transfer(uint64_t offset, void *data, size_t bytes, unsigned write);
int block_flush(void);
void block_report(void);
#endif
