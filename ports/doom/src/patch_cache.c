#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include "patch_cache.h"
#include "wad_io.h"
#include "w_wad.h"
#include "z_zone.h"
#include "i_system.h"

static patch_t **patches;

const char *Doom_ValidatePatch(const byte *raw, uint32_t length)
{
    uint16_t width, height, x;
    uint32_t directory_end, column, at, post_size;
    size_t header, payload;
    if (raw == NULL) return "Null patch data";
    /* Offsets in the decoded header are signed 32-bit C storage units. Reject
     * the bound before subtracting payload size or accessing any payload. */
    if (length > (uint32_t) INT32_MAX) return "Patch too large";
    if (length < 8UL || !wad_u16(raw, length, 0, &width)
        || !wad_u16(raw, length, 2, &height)) return "Truncated patch header";
    if (!width || width > 32767U || !height || height > 32767U)
        return "Invalid patch dimensions";
    directory_end = 8UL + (uint32_t) width * 4UL;
    if (directory_end > length) return "Truncated patch directory";
    header = offsetof(patch_t, columnofs)
           + (size_t) (width < 8U ? 8U : width) * sizeof(((patch_t *) 0)->columnofs[0]);
    payload = (size_t) (length - directory_end);
    if (payload > (size_t) INT32_MAX || header > (size_t) INT32_MAX - payload)
        return "Patch too large";
    for (x = 0; x < width; ++x) {
        if (!wad_u32(raw, length, 8UL + (uint32_t) x * 4UL, &column)
            || column < directory_end || column >= length)
            return "Invalid patch column";
        at = column;
        while (raw[at] != 255U) {
            if (!wad_range(length, at, 4UL)) return "Truncated patch post";
            post_size = 4UL + (uint32_t) raw[at + 1UL];
            /* Include the next post's marker in the range. Subtraction-based
             * checking ensures that advancing at cannot wrap around. */
            if (!wad_range(length, at, post_size + 1UL)
                || (uint32_t) raw[at] + raw[at + 1UL] > height)
                return "Invalid patch post";
            at += post_size;
        }
    }
    return NULL;
}

static uint16_t read16(const byte *raw, uint32_t length, uint32_t at)
{
    uint16_t value;
    if (!wad_u16(raw, length, at, &value)) I_Error("Truncated patch header");
    return value;
}

static uint32_t read32(const byte *raw, uint32_t length, uint32_t at)
{
    uint32_t value;
    if (!wad_u32(raw, length, at, &value)) I_Error("Truncated patch column table");
    return value;
}

patch_t *W_CachePatchNum(int lump, int tag)
{
    byte *raw;
    uint32_t length, directory_end, column;
    size_t header, payload;
    uint16_t width, height, x;
    patch_t *patch;
    const char *error;
    if (lump < 0 || (unsigned int)lump >= numlumps) I_Error("Bad patch lump %d", lump);
    if (!patches) {
        patches = calloc((size_t)numlumps, sizeof(*patches));
        if (!patches) I_Error("Cannot allocate patch cache");
    }
    if (patches[lump]) {
        Z_ChangeTag(patches[lump], tag);
        return patches[lump];
    }
    length = W_LumpLength(lump);
    raw = W_CacheLumpNum(lump, PU_STATIC);
    error = Doom_ValidatePatch(raw, length);
    if (error != NULL) I_Error("Invalid patch lump %d: %s", lump, error);
    width = read16(raw, length, 0);
    height = read16(raw, length, 2);
    directory_end = 8UL + (uint32_t)width * 4UL;
    /* Keep the historic columnofs[8] declaration valid for small patches. */
    header = offsetof(patch_t, columnofs)
           + (size_t)(width < 8U ? 8U : width) * sizeof(patch->columnofs[0]);
    payload = (size_t)(length - directory_end);
    patch = Z_Malloc(header + payload, tag, &patches[lump]);
    patch->width = (short)width;
    patch->height = (short)height;
    patch->leftoffset = (short)read16(raw, length, 4);
    patch->topoffset = (short)read16(raw, length, 6);
    for (x = 0; x < width; ++x) {
        column = read32(raw, length, 8UL + (uint32_t)x * 4UL);
        patch->columnofs[x] = (int32_t)(header + column - directory_end);
    }
    memcpy((byte *)patch + header, raw + directory_end, payload);
    W_ReleaseLumpNum(lump);
    return patch;
}

patch_t *W_CachePatchName(char *name, int tag)
{
    return W_CachePatchNum(W_GetNumForName(name), tag);
}

void W_ReleasePatchNum(int lump)
{
    if (lump < 0 || (unsigned int)lump >= numlumps) I_Error("Bad patch lump %d", lump);
    if (patches != NULL && patches[lump] != NULL)
        Z_ChangeTag(patches[lump], PU_CACHE);
}

void W_ReleasePatchName(char *name)
{
    W_ReleasePatchNum(W_GetNumForName(name));
}
