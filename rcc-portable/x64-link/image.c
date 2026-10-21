/* A large contiguous final image can fail in a fragmented bounded guest heap,
 * even when its live total fits. Code addresses remain offsets in one logical
 * image; physical storage and buffered output use independently owned chunks. */
#include "link.h"
#include <string.h>

#define IMAGE_CHUNK 65536UL
static unsigned char *chunks[LD_LIMIT / IMAGE_CHUNK];

void ld_image_allocate(void)
{
    L32 at;
    for (at = 0; at < ld_code_size; at += IMAGE_CHUNK) {
        L32 size = ld_code_size - at;
        if (size > IMAGE_CHUNK)
            size = IMAGE_CHUNK;
        chunks[at / IMAGE_CHUNK] = ld_memory(size);
    }
}

void ld_image_copy(L32 at, const unsigned char *data, L32 size)
{
    if (at > ld_code_size || size > ld_code_size - at)
        ld_error("copy outside native image");
    while (size) {
        L32 offset = at % IMAGE_CHUNK, count = IMAGE_CHUNK - offset;
        if (count > size)
            count = size;
        memcpy(chunks[at / IMAGE_CHUNK] + offset, data, count);
        at += count;
        data += count;
        size -= count;
    }
}

void ld_image_patch(L32 at, L64 value, unsigned width)
{
    if (!width || width > 8 || at > ld_code_size || width > ld_code_size - at)
        ld_error("patch outside native image");
    while (width--) {
        chunks[at / IMAGE_CHUNK][at % IMAGE_CHUNK] = value & 255;
        value >>= 8;
        at++;
    }
}

void ld_image_write(FILE *file)
{
    L32 at;
    for (at = 0; at < ld_code_size; at += IMAGE_CHUNK) {
        L32 size = ld_code_size - at;
        if (size > IMAGE_CHUNK)
            size = IMAGE_CHUNK;
        if (fwrite(chunks[at / IMAGE_CHUNK], 1, size, file) != size)
            ld_error("cannot write native image");
    }
}
