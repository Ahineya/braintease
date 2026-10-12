#ifndef DOOM_PORT_PATCH_CACHE_H
#define DOOM_PORT_PATCH_CACHE_H
#include "doomtype.h"
#include "v_patch.h"
/* Decoded patch headers and offsets use C memory units. Raw W_CacheLump*
 * remains an octet API for flats, palettes, maps, and serialized records. */
/* Returns NULL for a valid vanilla patch, otherwise a diagnostic. No allocation
 * or cache state changes, so malformed inputs can be tested on either host. */
const char *Doom_ValidatePatch(const byte *raw, uint32_t length);
patch_t *W_CachePatchNum(int lump, int tag);
patch_t *W_CachePatchName(char *name, int tag);
/* Release only decoded storage. Uncached or already-purged entries are safe;
 * the separate raw lump cache may have been purged since decoding. */
void W_ReleasePatchNum(int lump);
void W_ReleasePatchName(char *name);
#endif
