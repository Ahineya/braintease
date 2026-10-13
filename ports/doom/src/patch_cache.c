#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include "patch_cache.h"
#include "wad_io.h"
#include "w_wad.h"
#include "z_zone.h"
#include "i_system.h"

#ifdef RIPPLE
/* Cooked blocks are serialized C cells, so the direct-read path requires the
 * target ABI used by the packer rather than the host's byte layout. */
static void check_cooked_abi(void)
{
    if (sizeof(short) != 1 || sizeof(uint16_t) != 1 || sizeof(int32_t) != 2
        || offsetof(patch_t, columnofs) != 4)
        I_Error("Cooked patches do not match the target C ABI");
}
#endif

static patch_t **patches;
static patch_t **patch_headers;

static void check_lump(int lump)
{
    if (lump < 0 || (unsigned int)lump >= numlumps) I_Error("Bad patch lump %d", lump);
}

static size_t header_size(uint16_t width)
{
    return offsetof(patch_t, columnofs)
         + (size_t)(width < 8U ? 8U : width) * sizeof(((patch_t *)0)->columnofs[0]);
}

static void check_info(int lump, const DoomPatchInfo *info)
{
    if (info->width <= 0 || info->height <= 0)
        I_Error("Invalid patch dimensions in lump %d", lump);
}

static void copy_info(const patch_t *patch, DoomPatchInfo *info)
{
    info->width = patch->width;
    info->height = patch->height;
    info->leftoffset = patch->leftoffset;
    info->topoffset = patch->topoffset;
}

static void set_info(patch_t *patch, const DoomPatchInfo *info)
{
    patch->width = info->width;
    patch->height = info->height;
    patch->leftoffset = info->leftoffset;
    patch->topoffset = info->topoffset;
}

static void allocate_slots(patch_t ***slots)
{
    if (*slots == NULL) {
        *slots = calloc((size_t)numlumps, sizeof(**slots));
        if (*slots == NULL) I_Error("Cannot allocate patch cache");
    }
}

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

static void read_raw_part(int lump, uint32_t offset, byte *dest, uint32_t count)
{
    lumpinfo_t *entry = &lumpinfo[lump];
    if (!wad_range(entry->size, offset, count)
        || entry->position > UINT32_MAX - offset
        || W_Read(entry->wad_file, entry->position + offset, dest, (size_t)count) != count)
        I_Error("Cannot read patch header in lump %d", lump);
}

static void read_raw_info(int lump, DoomPatchInfo *info)
{
    byte raw[8];
    uint32_t length = W_LumpLength(lump);
    size_t header;
    uint32_t directory_end;
    read_raw_part(lump, 0, raw, 8UL);
    info->width = (short)read16(raw, 8UL, 0);
    info->height = (short)read16(raw, 8UL, 2);
    info->leftoffset = (short)read16(raw, 8UL, 4);
    info->topoffset = (short)read16(raw, 8UL, 6);
    check_info(lump, info);
    directory_end = 8UL + (uint32_t)info->width * 4UL;
    header = header_size((uint16_t)info->width);
    if (length < directory_end || length > (uint32_t)INT32_MAX
        || header > (size_t)INT32_MAX - (size_t)(length - directory_end))
        I_Error("Invalid patch header bounds in lump %d", lump);
}

#ifdef RIPPLE
static void read_cooked_info(int lump, uint32_t offset, uint32_t count, DoomPatchInfo *info)
{
    uint16_t words[4];
    check_cooked_abi();
    if (count < 20UL || count > (uint32_t)INT32_MAX)
        I_Error("Invalid cooked patch size in lump %d", lump);
    W_ReadAssetWords(offset, words, 4UL);
    info->width = (short)words[0];
    info->height = (short)words[1];
    info->leftoffset = (short)words[2];
    info->topoffset = (short)words[3];
    check_info(lump, info);
    if (header_size((uint16_t)info->width) > count)
        I_Error("Truncated cooked patch directory in lump %d", lump);
}

static void check_cooked_columns(int lump, const patch_t *patch, uint32_t count)
{
    uint16_t x;
    size_t header;
    if (patch->width <= 0 || patch->height <= 0)
        I_Error("Invalid cooked patch dimensions in lump %d", lump);
    header = header_size((uint16_t)patch->width);
    if (header > count) I_Error("Truncated cooked patch directory in lump %d", lump);
    for (x = 0; x < (uint16_t)patch->width; ++x) {
        int32_t column = patch->columnofs[x];
        if (column < 0 || (uint32_t)column < header || (uint32_t)column >= count)
            I_Error("Invalid cooked patch column in lump %d", lump);
    }
}
#endif

void W_ReadPatchInfoNum(int lump, DoomPatchInfo *info)
{
#ifdef RIPPLE
    uint32_t offset, count;
#endif
    check_lump(lump);
    if (patches != NULL && patches[lump] != NULL) {
        copy_info(patches[lump], info);
        return;
    }
    if (patch_headers != NULL && patch_headers[lump] != NULL) {
        copy_info(patch_headers[lump], info);
        return;
    }
#ifdef RIPPLE
    if (W_PatchAssetLocation(lump, &offset, &count)) {
        read_cooked_info(lump, offset, count, info);
        return;
    }
#endif
    read_raw_info(lump, info);
}

patch_t *W_CachePatchHeaderNum(int lump, int tag)
{
    DoomPatchInfo info;
    patch_t *patch;
    size_t header;
    uint32_t directory_end, length, column;
    uint16_t width, x;
    byte *columns;
#ifdef RIPPLE
    uint32_t offset, count;
#endif
    check_lump(lump);
    if (patches != NULL && patches[lump] != NULL) {
        Z_ChangeTag(patches[lump], tag);
        return patches[lump];
    }
    allocate_slots(&patch_headers);
    if (patch_headers[lump] != NULL) {
        Z_ChangeTag(patch_headers[lump], tag);
        return patch_headers[lump];
    }
#ifdef RIPPLE
    if (W_PatchAssetLocation(lump, &offset, &count)) {
        read_cooked_info(lump, offset, count, &info);
        header = header_size((uint16_t)info.width);
        patch = Z_Malloc(header, tag, &patch_headers[lump]);
        set_info(patch, &info);
        W_ReadAssetWords(offset + 4UL, (uint16_t *)patch + 4, (uint32_t)header - 4UL);
        check_cooked_columns(lump, patch, count);
        return patch;
    }
#endif
    read_raw_info(lump, &info);
    width = (uint16_t)info.width;
    header = header_size(width);
    directory_end = 8UL + (uint32_t)width * 4UL;
    length = W_LumpLength(lump);
    columns = malloc((size_t)width * 4UL);
    if (columns == NULL) I_Error("Cannot allocate patch column table");
    read_raw_part(lump, 8UL, columns, (uint32_t)width * 4UL);
    patch = Z_Malloc(header, tag, &patch_headers[lump]);
    memset(patch, 0, header);
    set_info(patch, &info);
    for (x = 0; x < width; ++x) {
        column = read32(columns, (uint32_t)width * 4UL, (uint32_t)x * 4UL);
        if (column < directory_end || column >= length)
            I_Error("Invalid patch column in lump %d", lump);
        patch->columnofs[x] = (int32_t)(header + column - directory_end);
    }
    free(columns);
    return patch;
}

patch_t *W_CachePatchNum(int lump, int tag)
{
    byte *raw;
    uint32_t length, directory_end, column;
    size_t header, payload;
    uint16_t width, height, x;
    patch_t *patch;
    const char *error;
#ifdef RIPPLE
    uint32_t offset, count;
#endif
    check_lump(lump);
    allocate_slots(&patches);
    if (patches[lump]) {
        Z_ChangeTag(patches[lump], tag);
        return patches[lump];
    }
#ifdef RIPPLE
    if (W_PatchAssetLocation(lump, &offset, &count)) {
        check_cooked_abi();
        if (count < 20UL || count > (uint32_t)INT32_MAX)
            I_Error("Invalid cooked patch size in lump %d", lump);
        patch = Z_Malloc((size_t)count, tag, &patches[lump]);
        W_ReadAssetWords(offset, (uint16_t *)patch, count);
        check_cooked_columns(lump, patch, count);
        return patch;
    }
#endif
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
    memset(patch, 0, header);
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
    check_lump(lump);
    if (patches != NULL && patches[lump] != NULL)
        Z_ChangeTag(patches[lump], PU_CACHE);
    if (patch_headers != NULL && patch_headers[lump] != NULL)
        Z_ChangeTag(patch_headers[lump], PU_CACHE);
}

void W_ReleasePatchName(char *name)
{
    W_ReleasePatchNum(W_GetNumForName(name));
}
