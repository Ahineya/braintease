#ifndef RIPLIX_ROOT_WINDOW_H
#define RIPLIX_ROOT_WINDOW_H
#include "gpt.h"
/* Project-private GPT type; never treat an ESP or Linux filesystem as a root.
 * GUID a90c10b4-cb9d-4a61-8b6a-e8f4d16938cf, in GPT byte order. */
#define RIPLIX_ROOT_TYPE {0xb4,0x10,0x0c,0xa9,0x9d,0xcb,0x61,0x4a,0x8b,0x6a,0xe8,0xf4,0xd1,0x69,0x38,0xcf}
typedef struct {
    uint8_t disk[16], partition[16];
    uint64_t first, last;
    unsigned sector;
} RootIdentity;
typedef struct { uint64_t offset, bytes; unsigned ready; } RootWindow;
/* Read-only validation, clearing the result on EVERY failure. Exact identity,
 * geometry, dedicated type, zero attributes and a bounded C16 V3 superblock
 * are required before callers may enable writes. No selection by label/index. */
const char *root_window_open(GptRead, void *, uint64_t, unsigned,
                             const RootIdentity *, RootWindow *);
int root_window_translate(const RootWindow *, uint64_t, size_t, uint64_t *);
int root_window_contains(const RootWindow *, uint64_t, uint64_t);
#endif
