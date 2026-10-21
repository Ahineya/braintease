#ifndef X64_ENTROPY_H
#define X64_ENTROPY_H
#include <stddef.h>
#include <stdint.h>
typedef int (*EntropyWord)(void *, uint64_t *);
/* Atomic bounded octet fill. No time/MAC/boot-seed fallback. */
int entropy_generate(uint8_t *, unsigned, EntropyWord, void *);
int entropy_available(void);
int entropy_fill(uint8_t *, unsigned);
#endif
