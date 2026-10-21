#ifndef RIPLIX_GPT_H
#define RIPLIX_GPT_H
#include <stddef.h>
#include <stdint.h>
#define GPT_MAX_PARTITIONS 128
typedef int (*GptRead)(void *, uint64_t offset, void *, size_t);
typedef struct {
    uint8_t type[16], guid[16];
    uint64_t first, last, attributes;
    unsigned index;
    char name[37];
} GptPartition;
typedef struct {
    uint8_t guid[16];
    uint64_t first_usable, last_usable;
    unsigned count, sector_size;
    GptPartition partitions[GPT_MAX_PARTITIONS];
} GptInfo;
/* Strict read-only discovery: require BOTH headers and entry arrays valid and
 * mutually consistent. Never repair GPT, guess a root or select a partition. */
const char *gpt_discover(GptRead read, void *ctx, uint64_t bytes, unsigned sector, GptInfo *out);
#endif
