#ifndef DOOM_PORT_ASSET_METADATA_H
#define DOOM_PORT_ASSET_METADATA_H

#include "w_wad.h"

/* Cooked metadata contains target C cells, not WAD octets. These return zero
 * (or NULL) on native and non-cooked builds so the original decoders remain
 * the reference path. Invalid cooked metadata is always a fatal error. */
int W_LoadAssetDirectory(wad_file_t *file, lumpinfo_t *dest, unsigned int count);
int W_LoadAssetHash(lumpinfo_t **heads);
/* The caller owns the returned PU_STATIC zone allocation, including an empty
 * lookup. Missing names have the same -1 value as W_CheckNumForName. */
int *W_LoadPatchNames(int pnames_lump, uint32_t *count);

#endif
