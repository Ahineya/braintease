/* SPDX-License-Identifier: GPL-2.0-or-later
 * Configured, bounded IWAD storage for Doom's existing file-class interface.
 * The same reader serves native validation and Ripple MMIO storage.
 */
#include <stdlib.h>
#include <string.h>
#include "doom_build_config.h"
#include "wad_io.h"
#include "w_file.h"
#include "w_wad.h"
#include "patch_cache.h"
#include "i_system.h"

typedef struct {
    wad_file_t base;
    WadFile input;
    uint16_t *patch_index;
} port_wad_file_t;

extern wad_file_class_t stdc_wad_file;
static port_wad_file_t *active_wad;

static wad_file_t *port_open(char *path) {
    port_wad_file_t *file;
    if (active_wad != NULL)
        I_Error("This port supports one configured IWAD; additional WADs are not supported");
    if (path == NULL || (strcmp(path, DOOM_WAD_PATH) != 0 && strcmp(path, "doom1.wad") != 0))
        I_Error("WAD path is not the configured fixture: %s", path ? path : "(null)");
    file = malloc(sizeof(port_wad_file_t));
    if (!file) I_Error("No memory for WAD file handle");
    /* W_AddFile validates and builds Doom's directory. Do not build a second,
     * unused WadLump index here before the engine reads the same records. */
    if (!wad_open_stream(&file->input, DOOM_WAD_PATH, DOOM_WAD_LENGTH)) {
        const char *error = file->input.error;
        free(file);
        I_Error("Cannot open configured WAD: %s", error);
        return NULL;
    }
    file->base.file_class = &stdc_wad_file;
    file->base.mapped = NULL;
    file->base.length = file->input.length;
    file->patch_index = NULL;
    active_wad = file;
    return &file->base;
}

static void port_close(wad_file_t *base) {
    port_wad_file_t *file = (port_wad_file_t *)base;
    wad_close(&file->input);
    free(file->patch_index);
    if (active_wad == file) active_wad = NULL;
    free(file);
}

static size_t port_read(wad_file_t *base, uint32_t offset, void *buffer, size_t count) {
    port_wad_file_t *file = (port_wad_file_t *)base;
    if (count > UINT32_MAX || !wad_read(&file->input, offset, (WadOctet *)buffer, (uint32_t)count))
        return 0;
    return count;
}

void W_ReadAssetWords(uint32_t word_offset, uint16_t *dest, uint32_t count)
{
    if (active_wad == NULL) I_Error("No WAD open for asset read");
    if (!wad_read_words(&active_wad->input, word_offset, dest, count))
        I_Error("Cannot read cooked asset: %s", active_wad->input.error);
}

int W_PatchAssetLocation(int lump, uint32_t *word_offset, uint32_t *word_count)
{
#if defined(RIPPLE) && DOOM_WAD_MODE == 2
    uint32_t index_words, index_end, at, offset, count;
    uint16_t *index;
    if (active_wad == NULL) I_Error("No WAD open for patch lookup");
    if (lump < 0 || (unsigned int)lump >= numlumps
        || (uint32_t)numlumps != DOOM_ASSET_LUMP_COUNT)
        I_Error("Cooked patch directory does not match WAD");
    if (DOOM_ASSET_LUMP_COUNT > UINT32_MAX / 4UL)
        I_Error("Cooked patch directory is too large");
    index_words = DOOM_ASSET_LUMP_COUNT * 4UL;
    if (!wad_range(DOOM_ASSET_TOTAL_WORDS, DOOM_ASSET_INDEX_WORD, index_words)
        || index_words > (uint32_t)(SIZE_MAX / sizeof(uint16_t)))
        I_Error("Invalid cooked patch directory bounds");
    index_end = DOOM_ASSET_PNAMES_WORD + DOOM_ASSET_PNAMES_COUNT;
    if (active_wad->patch_index == NULL) {
        /* This small permanent index also serves every sprite-header lookup.
         * Zone purges must not make repeated initialization reread it. */
        active_wad->patch_index = malloc((size_t)index_words * sizeof(uint16_t));
        if (active_wad->patch_index == NULL) I_Error("Cannot allocate cooked patch index");
        W_ReadAssetWords(DOOM_ASSET_INDEX_WORD, active_wad->patch_index, index_words);
    }
    index = active_wad->patch_index;
    at = (uint32_t)lump * 4UL;
    offset = (uint32_t)index[at] | ((uint32_t)index[at + 1UL] << 16);
    count = (uint32_t)index[at + 2UL] | ((uint32_t)index[at + 3UL] << 16);
    if (offset == 0 && count == 0) return 0;
    if (offset < index_end || count < 20UL || count > (uint32_t)INT32_MAX
        || !wad_range(DOOM_ASSET_TOTAL_WORDS, offset, count))
        I_Error("Invalid cooked patch location for lump %d", lump);
    *word_offset = offset;
    *word_count = count;
    return 1;
#else
    (void)lump; (void)word_offset; (void)word_count;
    return 0;
#endif
}

wad_file_class_t stdc_wad_file = {
    port_open,
    port_close,
    port_read
};
