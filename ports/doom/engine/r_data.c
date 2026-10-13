//
// Copyright(C) 1993-1996 Id Software, Inc.
// Copyright(C) 2005-2014 Simon Howard
//
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU General Public License
// as published by the Free Software Foundation; either version 2
// of the License, or (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// DESCRIPTION:
//	Preparation of data for rendering,
//	generation of lookups, caching, retrieval by name.
//

#include <stdio.h>

#include "deh_main.h"
#include "i_swap.h"
#include "i_system.h"
#include "z_zone.h"


#include "w_wad.h"

#include "doomdef.h"
#include "m_misc.h"
#include "r_local.h"
#include "p_local.h"

#include "doomstat.h"
#include "r_sky.h"


#include "r_data.h"
#include "patch_cache.h"
#include "doom_decode.h"
#include "asset_metadata.h"
#include "doom_build_config.h"
#include "texture_metadata.h"

/* Standalone builds keep the complete guest checks. The maintained launcher
 * can select the loader for an image validated and matched by native tools. */
#ifndef DOOM_ASSET_STRICT
#define DOOM_ASSET_STRICT 1
#endif

//
// Graphics.
// DOOM graphics for walls and sprites
// is stored in vertical runs of opaque pixels (posts).
// A column is composed of zero or more posts,
// a patch or sprite is composed of zero or more columns.
// 



//
// Texture definition.
// Each texture is composed of one or more patches,
// with patches being lumps stored in the WAD.
// The lumps are referenced by number, and patched
// into the rectangular texture space using origin
// and possibly other attributes.
//
typedef struct
{
    short	originx;
    short	originy;
    short	patch;
    short	stepdir;
    short	colormap;
} PACKEDATTR mappatch_t;


//
// Texture definition.
// A DOOM wall texture is a list of patches
// which are to be combined in a predefined order.
//
typedef struct
{
    char		name[8];
    int			masked;	
    short		width;
    short		height;
    int                 obsolete;
    short		patchcount;
    mappatch_t	patches[1];
} PACKEDATTR maptexture_t;


int		firstflat;
int		lastflat;
int		numflats;

int		firstpatch;
int		lastpatch;
int		numpatches;

int		firstspritelump;
int		lastspritelump;
int		numspritelumps;

int		numtextures;
texture_t**	textures;
texture_t**     textures_hashtable;


int*			texturewidthmask;
// needed for texture pegging
fixed_t*		textureheight;		
size_t*			texturecompositesize;
short**			texturecolumnlump;
uint32_t**	texturecolumnofs;
byte**			texturecomposite;

// for global animation
int*		flattranslation;
int*		texturetranslation;

// needed for pre rendering
fixed_t*	spritewidth;	
fixed_t*	spriteoffset;
fixed_t*	spritetopoffset;

lighttable_t	*colormaps;


//
// MAPTEXTURE_T CACHING
// When a texture is first needed,
//  it counts the number of composite columns
//  required in the texture and allocates space
//  for a column directory and any new columns.
// The directory will simply point inside other patches
//  if there is only one patch in a given column,
//  but any columns with multiple patches
//  will have new column_ts generated.
//



//
// R_DrawColumnInCache
// Clip and draw a column
//  from a patch into a cached post.
//
void
R_DrawColumnInCache
( column_t*	patch,
  byte*		cache,
  int		originy,
  int		cacheheight )
{
    int		count;
    int32_t		position;
    byte*	source;

    while (patch->topdelta != 0xff)
    {
	source = (byte *)patch + 3;
	count = patch->length;
	position = originy + patch->topdelta;

	if (position < 0)
	{
	    count += position;
	    position = 0;
	}

	if (position + count > cacheheight)
	    count = cacheheight - position;

	if (count > 0)
	    memcpy (cache + position, source, count);
		
	patch = (column_t *)(  (byte *)patch + patch->length + 4); 
    }
}



//
// R_GenerateComposite
// Using the texture definition,
//  the composite texture is created from the patches,
//  and each column is cached.
//
void R_GenerateComposite (int texnum)
{
    byte*		block;
    texture_t*		texture;
    texpatch_t*		patch;	
    patch_t*		realpatch;
    int			x;
    int32_t		x1;
    int32_t		x2;
    int			i;
    column_t*		patchcol;
    short*		collump;
    uint32_t*	colofs;
	
    texture = textures[texnum];

    block = Z_Malloc (texturecompositesize[texnum],
		      PU_STATIC, 
		      &texturecomposite[texnum]);	

    collump = texturecolumnlump[texnum];
    colofs = texturecolumnofs[texnum];
    /* Missing posts must not expose uninitialized zone memory. */
    memset(block, 0, texturecompositesize[texnum]);
    
    // Composite the columns together.
    patch = texture->patches;
		
    for (i=0 , patch = texture->patches;
	 i<texture->patchcount;
	 i++, patch++)
    {
	realpatch = W_CachePatchNum (patch->patch, PU_CACHE);
	x1 = patch->originx;
	x2 = x1 + SHORT(realpatch->width);

	if (x1<0)
	    x = 0;
	else
	    x = x1;
	
	if (x2 > texture->width)
	    x2 = texture->width;

	for ( ; x<x2 ; x++)
	{
	    // Column does not have multiple patches?
	    if (collump[x] >= 0)
		continue;
	    
	    patchcol = (column_t *)((byte *)realpatch
				    + LONG(realpatch->columnofs[x-x1]));
	    R_DrawColumnInCache (patchcol,
				 block + colofs[x],
				 patch->originy,
				 texture->height);
	}
						
    }

    /* The column samplers address rows 0..127 even for short textures. In
     * particular, projection rounding can sample row16 of a 16-row step.
     * Give every composite column its own complete sampling window, then
     * repeat the completed image instead of reading adjacent columns or zone
     * metadata. Keep this work in cache generation, outside the pixel loop. */
    if (texture->height < 128)
    {
        for (x = 0; x < texture->width; ++x)
        {
            byte *column;
            if (collump[x] >= 0) continue;
            column = block + colofs[x];
            for (i = texture->height; i < 128; ++i)
                column[i] = column[i - texture->height];
        }
    }

    // Now that the texture has been built in column cache,
    //  it is purgable from zone memory.
    Z_ChangeTag (block, PU_CACHE);
}



//
// R_GenerateLookup
//
void R_GenerateLookup (int texnum)
{
    texture_t*		texture;
    byte*		patchcount;	// patchcount[texture->width]
    texpatch_t*		patch;	
    patch_t*		realpatch;
    int			x;
    int32_t		x1;
    int32_t		x2;
    int			i;
    short*		collump;
    uint32_t*	colofs;
    size_t columnsize;
	
    texture = textures[texnum];
    columnsize = texture->height < 128 ? 128 : texture->height;

    // Composited texture not created yet.
    texturecomposite[texnum] = 0;
    
    texturecompositesize[texnum] = 0;
    collump = texturecolumnlump[texnum];
    colofs = texturecolumnofs[texnum];
    
    // Now count the number of columns
    //  that are covered by more than one patch.
    // Fill in the lump / offset, so columns
    //  with only a single patch are all done.
    patchcount = (byte *) Z_Malloc(texture->width, PU_STATIC, &patchcount);
    memset (patchcount, 0, texture->width);
    patch = texture->patches;

    for (i=0 , patch = texture->patches;
	 i<texture->patchcount;
	 i++, patch++)
    {
	realpatch = W_CachePatchHeaderNum (patch->patch, PU_CACHE);
	x1 = patch->originx;
	x2 = x1 + SHORT(realpatch->width);
	
	if (x1 < 0)
	    x = 0;
	else
	    x = x1;

	if (x2 > texture->width)
	    x2 = texture->width;
	for ( ; x<x2 ; x++)
	{
	    if (patchcount[x] < 2) ++patchcount[x];
	    collump[x] = patch->patch;
	    colofs[x] = LONG(realpatch->columnofs[x-x1])+3;
	}
    }
	
    for (x=0 ; x<texture->width ; x++)
    {
	if (!patchcount[x])
	{
            I_Error("R_GenerateLookup: column without a patch (%.8s)",
                    texture->name);
	}
	// I_Error ("R_GenerateLookup: column without a patch");
	
	if (patchcount[x] > 1)
	{
	    // Use the cached block.
	    collump[x] = -1;	
	    colofs[x] = texturecompositesize[texnum];
	    
	    if (texturecompositesize[texnum] > 0x10000-columnsize)
	    {
		I_Error ("R_GenerateLookup: texture %i is >64k",
			 texnum);
	    }
	    
	    texturecompositesize[texnum] += columnsize;
	}
    }

    Z_Free(patchcount);
}




//
// R_GetColumn
//
byte*
R_GetColumn
( int		tex,
  int		col )
{
    int		lump;
    uint32_t	ofs;
	
    col &= texturewidthmask[tex];
    lump = texturecolumnlump[tex][col];
    ofs = texturecolumnofs[tex][col];
    
    if (lump >= 0)
	return (byte *)W_CachePatchNum(lump,PU_CACHE)+ofs;

    if (!texturecomposite[tex])
	R_GenerateComposite (tex);

    return texturecomposite[tex] + ofs;
}


static void GenerateTextureHashTable(void)
{
    texture_t **rover;
    int i;
    int key;

    textures_hashtable 
            = Z_Malloc(sizeof(texture_t *) * numtextures, PU_STATIC, 0);

    memset(textures_hashtable, 0, sizeof(texture_t *) * numtextures);

    // Add all textures to hash table

    for (i=0; i<numtextures; ++i)
    {
        // Store index

        textures[i]->index = i;

        // Vanilla Doom does a linear search of the texures array
        // and stops at the first entry it finds.  If there are two
        // entries with the same name, the first one in the array
        // wins. The new entry must therefore be added at the end
        // of the hash chain, so that earlier entries win.

        key = W_LumpNameHash(textures[i]->name) % numtextures;

        rover = &textures_hashtable[key];

        while (*rover != NULL)
        {
            rover = &(*rover)->next;
        }

        // Hook into hash table

        textures[i]->next = NULL;
        *rover = textures[i];
    }
}

#if defined(RIPPLE) && DOOM_WAD_MODE == 2
static uint32_t CookedTexturePair(const uint16_t *words)
{
    uint32_t value;
    /* The image stores native target cells; copy the pair without bitwise
     * byte decoding. This is only compiled for the checked Ripple ABI. */
    memcpy(&value, words, sizeof(value));
    return value;
}

#if DOOM_ASSET_STRICT
static uint32_t CookedTexturePatchSize(int lump, int *previous,
                                      uint32_t *previous_size)
{
    uint32_t location;
    if (lump < 0 || (unsigned int)lump >= numlumps)
        I_Error("Invalid cooked texture patch ID");
    if (lump != *previous) {
        if (!W_PatchAssetLocation(lump, &location, previous_size))
            I_Error("Cooked texture refers to a raw-only patch");
        *previous = lump;
    }
    return *previous_size;
}
#endif

static int R_LoadCookedTextures(void)
{
    uint16_t header[16];
    uint16_t *slab, *cursor, *definitions, *columns, *offsets;
    uint16_t *masks, *heights, *sizes, *links;
    uint32_t definition_words, column_count, tail_words, slab_words;
    uint32_t definitions_at, columns_at, offsets_at, masks_at, heights_at, sizes_at, links_at;
    uint32_t remaining_definitions, remaining_columns, file_at;
#if DOOM_ASSET_STRICT
    uint32_t previous_size = 0;
    int previous_patch = -1;
#endif
    unsigned int count, i;

    if (sizeof(char) != 1 || sizeof(short) != 1 || sizeof(int) != 1
        || sizeof(void *) != 2 || sizeof(uint32_t) != 2 || sizeof(size_t) != 2
        || sizeof(fixed_t) != 2 || sizeof(texpatch_t) != 3
        || sizeof(texture_t) != 17 || offsetof(texture_t, next) != 11
        || offsetof(texture_t, patchcount) != 13 || offsetof(texture_t, patches) != 14)
        I_Error("Cooked textures do not match the target C ABI");
    if (DOOM_ASSET_TEXTURE_COUNT == 0UL || DOOM_ASSET_TEXTURE_COUNT > 32767UL
        || numlumps == 0 || (uint32_t)numlumps != DOOM_ASSET_LUMP_COUNT
        || DOOM_ASSET_TEXTURE_WORDS < 16UL
        || DOOM_ASSET_TEXTURE_WORD > DOOM_ASSET_TOTAL_WORDS
        || DOOM_ASSET_TEXTURE_WORDS > DOOM_ASSET_TOTAL_WORDS - DOOM_ASSET_TEXTURE_WORD)
        I_Error("Invalid cooked texture section bounds");

    W_ReadAssetWords(DOOM_ASSET_TEXTURE_WORD, header, 16UL);
    count = (unsigned int)DOOM_ASSET_TEXTURE_COUNT;
    definition_words = CookedTexturePair(header + 4);
    column_count = CookedTexturePair(header + 6);
    if (header[0] != 0x5458U || header[1] != 1U || header[2] != count
        || header[3] != 0 || definition_words != DOOM_ASSET_TEXTURE_DEFINITION_WORDS
        || column_count != DOOM_ASSET_TEXTURE_COLUMNS
        || header[8] != (unsigned int)W_GetNumForName("TEXTURE1")
        || header[9] != (unsigned int)W_CheckNumForName("TEXTURE2")
        || header[10] != (unsigned int)W_GetNumForName("PNAMES"))
        I_Error("Cooked texture header does not match the WAD");
    for (i = 11; i < 16; ++i)
        if (header[i] != 0) I_Error("Invalid cooked texture header padding");
    tail_words = DOOM_ASSET_TEXTURE_WORDS - 16UL;
    if (definition_words < (uint32_t)count * 17UL || definition_words > tail_words
        || (uint32_t)count * 6UL > tail_words - definition_words
        || column_count > (tail_words - definition_words - (uint32_t)count * 6UL) / 3UL
        || definition_words + column_count * 3UL + (uint32_t)count * 6UL != tail_words
        || tail_words > (uint32_t)SIZE_MAX - (uint32_t)count * 10UL - 1UL)
        I_Error("Invalid cooked texture section layout");

    /* All texture definitions, column tables and mutable pointer tables live
     * in one permanent zone block. The final serialized hash IDs are reused
     * as the animation translation table after the hash pointers are built. */
    slab_words = (uint32_t)count * 10UL + tail_words + 1UL;
    slab = Z_Malloc((size_t)slab_words, PU_STATIC, NULL);
    cursor = slab;
    textures = (texture_t **)cursor; cursor += (size_t)count * 2U;
    texturecolumnlump = (short **)cursor; cursor += (size_t)count * 2U;
    texturecolumnofs = (uint32_t **)cursor; cursor += (size_t)count * 2U;
    texturecomposite = (byte **)cursor; cursor += (size_t)count * 2U;
    textures_hashtable = (texture_t **)cursor; cursor += (size_t)count * 2U;
    definitions = cursor; cursor += definition_words;
    columns = cursor; cursor += column_count;
    offsets = cursor; cursor += column_count * 2UL;
    masks = cursor; cursor += count;
    heights = cursor; cursor += (size_t)count * 2U;
    sizes = cursor; cursor += (size_t)count * 2U;
    links = cursor;
    texturewidthmask = (int *)masks;
    textureheight = (fixed_t *)heights;
    texturecompositesize = (size_t *)sizes;
    texturetranslation = (int *)links;
    numtextures = count;

    definitions_at = DOOM_ASSET_TEXTURE_WORD + 16UL;
    columns_at = definitions_at + definition_words;
    offsets_at = columns_at + column_count;
    masks_at = offsets_at + column_count * 2UL;
    heights_at = masks_at + count;
    sizes_at = heights_at + (uint32_t)count * 2UL;
    links_at = sizes_at + (uint32_t)count * 2UL;
    file_at = definitions_at;
    remaining_definitions = definition_words;
    remaining_columns = column_count;
    cursor = definitions;
    for (i = 0; i < count; ++i) {
        texture_t record;
#if DOOM_ASSET_STRICT
        texpatch_t patch_records[32];
#endif
        texture_t *texture = (texture_t *)cursor;
        uint16_t *cells = (uint16_t *)&record;
        unsigned int next;
#if DOOM_ASSET_STRICT
        unsigned int j, remaining_patches;
#endif
        uint32_t record_words;
        if (remaining_definitions < 14UL)
            I_Error("Truncated cooked texture definition");
        W_ReadAssetWords(file_at, cells, 14UL);
        /* Width and patch count determine slab offsets even in the preflight
         * path. Bound them before advancing any destination pointer. */
        if (record.width <= 0 || record.patchcount <= 0)
            I_Error("Invalid cooked texture definition");
        next = cells[11];
#if DOOM_ASSET_STRICT
        if (record.height <= 0 || record.index != (int)i)
            I_Error("Invalid cooked texture definition");
        for (j = 0; j < 8; ++j)
            if ((unsigned int)record.name[j] > 255U)
                I_Error("Invalid cooked texture name");
        if (cells[12] != 0 || (next < count ? next <= i : next != 65535U))
            I_Error("Invalid cooked texture hash link");
#endif
        record_words = 14UL + (uint32_t)record.patchcount * 3UL;
        if (record_words > remaining_definitions || (uint32_t)record.width > remaining_columns)
            I_Error("Cooked texture definition exceeds its section");
        texturetranslation[i] = (int)next;
        record.next = NULL;
        textures[i] = texture;
        texturecolumnlump[i] = (short *)columns;
        texturecolumnofs[i] = (uint32_t *)offsets;
        texturecomposite[i] = NULL;
        memcpy(cursor, &record, 14U);
        cursor += 14U;
        file_at += 14UL;
#if DOOM_ASSET_STRICT
        remaining_patches = (unsigned int)record.patchcount;
        while (remaining_patches != 0) {
            unsigned int batch = remaining_patches < 32U ? remaining_patches : 32U;
            unsigned int words = batch * 3U;
            W_ReadAssetWords(file_at, (uint16_t *)patch_records, (uint32_t)words);
            for (j = 0; j < batch; ++j)
                CookedTexturePatchSize(patch_records[j].patch, &previous_patch, &previous_size);
            memcpy(cursor, patch_records, (size_t)words);
            cursor += words;
            file_at += words;
            remaining_patches -= batch;
        }
#else
        /* The native preflight already checked every referenced patch. These
         * records have their final target layout and need no relocation. */
        W_ReadAssetWords(file_at, cursor, record_words - 14UL);
        cursor += record_words - 14UL;
        file_at += record_words - 14UL;
#endif
        columns += (unsigned int)record.width;
        offsets += (size_t)(unsigned int)record.width * 2U;
        remaining_definitions -= record_words;
        remaining_columns -= (unsigned int)record.width;
    }
    if (remaining_definitions != 0 || remaining_columns != 0)
        I_Error("Cooked texture section has unused definitions or columns");
    for (i = 0; i < count; ++i) {
        unsigned int next = (unsigned int)texturetranslation[i];
        if (next < count) textures[i]->next = textures[next];
    }

#if DOOM_ASSET_STRICT
    /* Scalar tables are small, but still validate near stack cells instead
     * of repeatedly traversing the distant permanent storage on Brainfuck. */
    for (i = 0; i < count;) {
        uint16_t batch_masks[32], batch_heights[64];
        uint32_t batch_sizes[32];
        unsigned int batch = count - i < 32U ? count - i : 32U;
        unsigned int j;
        W_ReadAssetWords(masks_at, batch_masks, (uint32_t)batch);
        W_ReadAssetWords(heights_at, batch_heights, (uint32_t)batch * 2UL);
        W_ReadAssetWords(sizes_at, (uint16_t *)batch_sizes, (uint32_t)batch * 2UL);
        for (j = 0; j < batch; ++j) {
            texture_t *texture = textures[i + j];
            unsigned int mask = 1U;
            unsigned int half_width = (unsigned int)texture->width / 2U;
            while (mask <= half_width) mask += mask;
            if (batch_masks[j] != mask - 1U || batch_heights[j * 2U] != 0
                || batch_heights[j * 2U + 1U] != (unsigned int)texture->height
                || batch_sizes[j] > 65536UL)
                I_Error("Invalid cooked texture scalar table");
        }
        memcpy(texturewidthmask + i, batch_masks, (size_t)batch);
        memcpy(textureheight + i, batch_heights, (size_t)batch * 2U);
        memcpy(texturecompositesize + i, batch_sizes, (size_t)batch * 2U);
        masks_at += batch;
        heights_at += (uint32_t)batch * 2UL;
        sizes_at += (uint32_t)batch * 2UL;
        i += batch;
    }

    for (i = 0; i < count; ++i) {
        short batch_lumps[32];
        uint32_t batch_offsets[32];
        texture_t *texture = textures[i];
        unsigned int remaining = (unsigned int)texture->width;
        unsigned int column = 0;
        uint32_t composite_size = 0;
        uint32_t column_size = texture->height < 128 ? 128UL : (uint32_t)texture->height;
        while (remaining != 0) {
            unsigned int batch = remaining < 32U ? remaining : 32U;
            unsigned int j;
            W_ReadAssetWords(columns_at, (uint16_t *)batch_lumps, (uint32_t)batch);
            W_ReadAssetWords(offsets_at, (uint16_t *)batch_offsets, (uint32_t)batch * 2UL);
            for (j = 0; j < batch; ++j) {
                int lump = batch_lumps[j];
                uint32_t offset = batch_offsets[j];
                if (lump < 0) {
                    if (lump != -1 || offset != composite_size
                        || column_size > 65536UL - composite_size)
                        I_Error("Invalid cooked composite column");
                    composite_size += column_size;
                } else {
                    uint32_t patch_size = CookedTexturePatchSize(lump, &previous_patch, &previous_size);
                    /* Empty posts may consist solely of a final 255 marker.
                     * Masked consumers subtract three from this pixel pointer
                     * before checking that marker, so the pixel pointer itself
                     * can legitimately lie just beyond the patch payload. */
                    if (offset < 3UL || offset - 3UL >= patch_size)
                        I_Error("Invalid cooked direct column offset");
                }
            }
            memcpy(texturecolumnlump[i] + column, batch_lumps, (size_t)batch);
            memcpy(texturecolumnofs[i] + column, batch_offsets, (size_t)batch * 2U);
            columns_at += batch;
            offsets_at += (uint32_t)batch * 2UL;
            column += batch;
            remaining -= batch;
        }
        if (composite_size != texturecompositesize[i])
            I_Error("Cooked composite size does not match its columns");
    }
#else
    /* Column IDs/offsets and scalar tables contain no guest pointers. Their
     * contiguous serialized layout already matches the allocated slab, and
     * native preflight has checked every entry. The storage reader handles
     * disk blocks and RAM banks while copying directly to permanent storage. */
    W_ReadAssetWords(columns_at, definitions + definition_words,
                    column_count * 3UL + (uint32_t)count * 5UL);
#endif

    for (i = 0; i < count;) {
        uint16_t heads[32];
        texture_t *pointers[32];
        unsigned int batch = count - i < 32U ? count - i : 32U;
        unsigned int j;
        W_ReadAssetWords(links_at, heads, (uint32_t)batch);
        for (j = 0; j < batch; ++j) {
            unsigned int index = heads[j];
            if (index < count) pointers[j] = textures[index];
            else {
#if DOOM_ASSET_STRICT
                if (index != 65535U) I_Error("Invalid cooked texture hash head");
#endif
                pointers[j] = NULL;
            }
            heads[j] = i + j;
        }
        memcpy(textures_hashtable + i, pointers, (size_t)batch * 2U);
        memcpy(texturetranslation + i, heads, (size_t)batch);
        links_at += batch;
        i += batch;
    }
    texturetranslation[count] = count;
    return 1;
}
#endif


//
// R_InitTextures
// Initializes the texture list
//  with the textures from the world map.
//
/* Serialized texture records always use file octets, independent of CHAR_BIT. */
void R_InitTextures(void)
{
    byte *names, *data;
    uint32_t names_len, count, length, offset, pcount, n, j;
    int *lookup;
    int lump, pass, index = 0;
    int texture_lumps[2];
    int counts[2];
    char name[9];
    texture_t *texture;
    texpatch_t *patch;
    int mask;

#if defined(RIPPLE) && DOOM_WAD_MODE == 2
    if (R_LoadCookedTextures()) return;
#endif

    lump = W_GetNumForName("PNAMES");
    lookup = W_LoadPatchNames(lump, &count);
    if (lookup == NULL)
    {
        names_len = W_LumpLength(lump);
        names = W_CacheLumpNum(lump, PU_STATIC);
        if (names_len < 4UL) I_Error("Truncated PNAMES");
        count = DoomReadLE32(names);
        if (count > 32767UL || count > (names_len - 4UL) / 8UL)
            I_Error("Invalid PNAMES count");
        lookup = Z_Malloc((size_t)count * sizeof(*lookup), PU_STATIC, NULL);
        for (n = 0; n < count; ++n) {
            DoomReadName8(names + 4UL + n * 8UL, name);
            lookup[n] = W_CheckNumForName(name);
        }
        W_ReleaseLumpNum(lump);
    }
    texture_lumps[0] = W_GetNumForName("TEXTURE1");
    texture_lumps[1] = W_CheckNumForName("TEXTURE2");
    numtextures = 0;
    for (pass = 0; pass < 2; ++pass) {
        counts[pass] = 0;
        if (texture_lumps[pass] < 0) continue;
        length = W_LumpLength(texture_lumps[pass]);
        data = W_CacheLumpNum(texture_lumps[pass], PU_STATIC);
        if (length < 4UL) I_Error("Truncated texture directory");
        n = DoomReadLE32(data);
        if (n > (uint32_t)(32767 - numtextures) || n > (length - 4UL) / 4UL)
            I_Error("Invalid texture count");
        counts[pass] = (int)n;
        numtextures += counts[pass];
    }
    if (!numtextures) I_Error("No textures");
    textures = Z_Malloc((size_t)numtextures * sizeof(*textures), PU_STATIC, NULL);
    texturecolumnlump = Z_Malloc((size_t)numtextures * sizeof(*texturecolumnlump), PU_STATIC, NULL);
    texturecolumnofs = Z_Malloc((size_t)numtextures * sizeof(*texturecolumnofs), PU_STATIC, NULL);
    texturecomposite = Z_Malloc((size_t)numtextures * sizeof(*texturecomposite), PU_STATIC, NULL);
    texturecompositesize = Z_Malloc((size_t)numtextures * sizeof(*texturecompositesize), PU_STATIC, NULL);
    texturewidthmask = Z_Malloc((size_t)numtextures * sizeof(*texturewidthmask), PU_STATIC, NULL);
    textureheight = Z_Malloc((size_t)numtextures * sizeof(*textureheight), PU_STATIC, NULL);
    for (pass = 0; pass < 2; ++pass) {
        if (texture_lumps[pass] < 0) continue;
        length = W_LumpLength(texture_lumps[pass]);
        data = W_CacheLumpNum(texture_lumps[pass], PU_STATIC);
        for (n = 0; n < (uint32_t)counts[pass]; ++n, ++index) {
            offset = DoomReadLE32(data + 4UL + n * 4UL);
            if (offset < 4UL + (uint32_t)counts[pass] * 4UL || offset > length || length - offset < 22UL)
                I_Error("Invalid texture offset");
            pcount = DoomReadLE16(data + offset + 20UL);
            if (!pcount || pcount > 32767UL || pcount > (length - offset - 22UL) / 10UL)
                I_Error("Invalid texture patches");
            texture = Z_Malloc(sizeof(*texture) + (size_t)(pcount - 1UL) * sizeof(texpatch_t), PU_STATIC, NULL);
            textures[index] = texture;
            memcpy(texture->name, data + offset, 8);
            texture->width = (short)DoomReadLE16(data + offset + 12UL);
            texture->height = (short)DoomReadLE16(data + offset + 14UL);
            texture->patchcount = (short)pcount;
            if (texture->width <= 0 || texture->height <= 0) I_Error("Invalid texture size");
            for (j = 0; j < pcount; ++j) {
                byte *record = data + offset + 22UL + j * 10UL;
                uint32_t pindex = DoomReadLE16(record + 4);
                if (pindex >= count || lookup[pindex] < 0) I_Error("Missing texture patch");
                patch = &texture->patches[j];
                patch->originx = (short)DoomReadS16(record);
                patch->originy = (short)DoomReadS16(record + 2);
                patch->patch = lookup[pindex];
            }
            texturecolumnlump[index] = Z_Malloc((size_t)texture->width * sizeof(**texturecolumnlump), PU_STATIC, NULL);
            texturecolumnofs[index] = Z_Malloc((size_t)texture->width * sizeof(**texturecolumnofs), PU_STATIC, NULL);
            mask = 1;
            while (mask <= texture->width / 2) mask *= 2;
            texturewidthmask[index] = mask - 1;
            textureheight[index] = (fixed_t)texture->height * FRACUNIT;
        }
        W_ReleaseLumpNum(texture_lumps[pass]);
    }
    Z_Free(lookup);
    for (index = 0; index < numtextures; ++index) R_GenerateLookup(index);
    texturetranslation = Z_Malloc(((size_t)numtextures + 1) * sizeof(*texturetranslation), PU_STATIC, NULL);
    for (index = 0; index < numtextures; ++index) texturetranslation[index] = index;
    GenerateTextureHashTable();
}


//
// R_InitFlats
//
void R_InitFlats (void)
{
    int		i;
	
    firstflat = W_GetNumForName (DEH_String("F_START")) + 1;
    lastflat = W_GetNumForName (DEH_String("F_END")) - 1;
    numflats = lastflat - firstflat + 1;
	
    // Create translation table for global animation.
    flattranslation = Z_Malloc ((numflats+1)*sizeof(*flattranslation), PU_STATIC, 0);
    
    for (i=0 ; i<numflats ; i++)
	flattranslation[i] = i;
}


#if defined(RIPPLE) && DOOM_WAD_MODE == 2
static int R_LoadCookedSpriteMetrics(void)
{
    uint16_t header[16];
    uint32_t array_words, payload_words;
    unsigned int i;
    fixed_t *slab;

    if (modifiedgame) return 0;
    if (sizeof(uint16_t) != 1 || sizeof(fixed_t) != 2 || FRACBITS != 16)
        I_Error("Cooked sprite metrics do not match the target C ABI");
    if (numspritelumps < 0 || DOOM_ASSET_SPRITE_METRICS_WORDS < 16UL
        || DOOM_ASSET_SPRITE_METRICS_WORD > DOOM_ASSET_TOTAL_WORDS
        || DOOM_ASSET_SPRITE_METRICS_WORDS > DOOM_ASSET_TOTAL_WORDS - DOOM_ASSET_SPRITE_METRICS_WORD)
        I_Error("Invalid cooked sprite metrics bounds");
    array_words = (uint32_t)numspritelumps * 2UL;
    payload_words = array_words * 3UL;
    if (DOOM_ASSET_SPRITE_METRICS_WORDS != 16UL + payload_words
        || payload_words > (uint32_t)SIZE_MAX)
        I_Error("Invalid cooked sprite metrics layout");
    W_ReadAssetWords(DOOM_ASSET_SPRITE_METRICS_WORD, header, 16UL);
    if (header[0] != 0x534dU || header[1] != 1U
        || header[2] != (unsigned int)numspritelumps
        || header[3] != (unsigned int)firstspritelump
        || header[4] != (unsigned int)lastspritelump || header[5] != 2U
        || header[6] != (unsigned int)array_words || header[7] != 0)
        I_Error("Cooked sprite metrics do not match the WAD");
    for (i = 8; i < 16; ++i)
        if (header[i] != 0) I_Error("Invalid cooked sprite metrics padding");
    if (!numspritelumps) {
        spritewidth = NULL;
        spriteoffset = NULL;
        spritetopoffset = NULL;
        return 1;
    }
    /* Final native arrays share one permanent block. No patch-index searches,
     * tiny header reads, fixed-point conversions, or per-array allocations. */
    slab = Z_Malloc((size_t)payload_words, PU_STATIC, NULL);
    spritewidth = slab;
    spriteoffset = slab + numspritelumps;
    spritetopoffset = spriteoffset + numspritelumps;
    W_ReadAssetWords(DOOM_ASSET_SPRITE_METRICS_WORD + 16UL,
                    (uint16_t *)slab, payload_words);
#if DOOM_ASSET_STRICT
    for (i = 0; i < (unsigned int)numspritelumps; ++i) {
        uint16_t *width = (uint16_t *)(spritewidth + i);
        uint16_t *left = (uint16_t *)(spriteoffset + i);
        uint16_t *top = (uint16_t *)(spritetopoffset + i);
        if (width[0] != 0 || width[1] == 0 || width[1] > 32767U
            || left[0] != 0 || top[0] != 0)
            I_Error("Invalid cooked sprite metric value");
    }
#endif
    /* Keep Doom's existing progress dots, now after the bulk transfer. */
    for (i = 0; i < (unsigned int)numspritelumps; i += 64U) printf(".");
    return 1;
}
#endif

//
// R_InitSpriteLumps
// Finds the width and hoffset of all sprites in the wad,
//  so the sprite does not need to be cached completely
//  just for having the header info ready during rendering.
//
void R_InitSpriteLumps (void)
{
    int		i;
    DoomPatchInfo patch;
	
    firstspritelump = W_GetNumForName (DEH_String("S_START")) + 1;
    lastspritelump = W_GetNumForName (DEH_String("S_END")) - 1;
    
    numspritelumps = lastspritelump - firstspritelump + 1;
#if defined(RIPPLE) && DOOM_WAD_MODE == 2
    if (R_LoadCookedSpriteMetrics()) return;
#endif
    spritewidth = Z_Malloc (numspritelumps*sizeof(*spritewidth), PU_STATIC, 0);
    spriteoffset = Z_Malloc (numspritelumps*sizeof(*spriteoffset), PU_STATIC, 0);
    spritetopoffset = Z_Malloc (numspritelumps*sizeof(*spritetopoffset), PU_STATIC, 0);
	
    for (i=0 ; i< numspritelumps ; i++)
    {
	if (!(i&63))
	    printf (".");

	W_ReadPatchInfoNum (firstspritelump+i, &patch);
	spritewidth[i] = (fixed_t)patch.width * FRACUNIT;
	spriteoffset[i] = (fixed_t)patch.leftoffset * FRACUNIT;
	spritetopoffset[i] = (fixed_t)patch.topoffset * FRACUNIT;
    }
}



//
// R_InitColormaps
//
void R_InitColormaps (void)
{
    int	lump;

    // Load in the light tables, 
    //  256 byte align tables.
    lump = W_GetNumForName(DEH_String("COLORMAP"));
    colormaps = W_CacheLumpNum(lump, PU_STATIC);
}



//
// R_InitData
// Locates all the lumps
//  that will be used by all views
// Must be called after W_Init.
//
void R_InitData (void)
{
    R_InitTextures ();
    printf (".");
    R_InitFlats ();
    printf (".");
    R_InitSpriteLumps ();
    printf (".");
    R_InitColormaps ();
}



//
// R_FlatNumForName
// Retrieval, get a flat number for a flat name.
//
int R_FlatNumForName (char* name)
{
    int		i;
    char	namet[9];

    i = W_CheckNumForName (name);

    if (i == -1)
    {
	namet[8] = 0;
	memcpy (namet, name,8);
	I_Error ("R_FlatNumForName: %s not found",namet);
    }
    return i - firstflat;
}




//
// R_CheckTextureNumForName
// Check whether texture is available.
// Filter out NoTexture indicator.
//
int	R_CheckTextureNumForName (char *name)
{
    texture_t *texture;
    int key;

    // "NoTexture" marker.
    if (name[0] == '-')		
	return 0;
		
    key = W_LumpNameHash(name) % numtextures;

    texture=textures_hashtable[key]; 
    
    while (texture != NULL)
    {
	if (!strncasecmp (texture->name, name, 8) )
	    return texture->index;

        texture = texture->next;
    }
    
    return -1;
}



//
// R_TextureNumForName
// Calls R_CheckTextureNumForName,
//  aborts with error message.
//
int	R_TextureNumForName (char* name)
{
    int		i;
	
    i = R_CheckTextureNumForName (name);

    if (i==-1)
    {
	I_Error ("R_TextureNumForName: %s not found",
		 name);
    }
    return i;
}




//
// R_PrecacheLevel
// Preloads all relevant graphics for the level.
//
int		flatmemory;
int		texturememory;
int		spritememory;

void R_PrecacheLevel (void)
{
    char*		flatpresent;
    char*		texturepresent;
    char*		spritepresent;

    int			i;
    int			j;
    int			k;
    int			lump;
    
    texture_t*		texture;
    thinker_t*		th;
    spriteframe_t*	sf;

    if (demoplayback)
	return;
    
    // Precache flats.
    flatpresent = Z_Malloc(numflats, PU_STATIC, NULL);
    memset (flatpresent,0,numflats);	

    for (i=0 ; i<numsectors ; i++)
    {
	flatpresent[sectors[i].floorpic] = 1;
	flatpresent[sectors[i].ceilingpic] = 1;
    }
	
    flatmemory = 0;

    for (i=0 ; i<numflats ; i++)
    {
	if (flatpresent[i])
	{
	    lump = firstflat + i;
	    flatmemory += lumpinfo[lump].size;
	    W_CacheLumpNum(lump, PU_CACHE);
	}
    }

    Z_Free(flatpresent);
    
    // Precache textures.
    texturepresent = Z_Malloc(numtextures, PU_STATIC, NULL);
    memset (texturepresent,0, numtextures);
	
    for (i=0 ; i<numsides ; i++)
    {
	texturepresent[sides[i].toptexture] = 1;
	texturepresent[sides[i].midtexture] = 1;
	texturepresent[sides[i].bottomtexture] = 1;
    }

    // Sky texture is always present.
    // Note that F_SKY1 is the name used to
    //  indicate a sky floor/ceiling as a flat,
    //  while the sky texture is stored like
    //  a wall texture, with an episode dependend
    //  name.
    texturepresent[skytexture] = 1;
	
    texturememory = 0;
    for (i=0 ; i<numtextures ; i++)
    {
	if (!texturepresent[i])
	    continue;

	texture = textures[i];
	
	for (j=0 ; j<texture->patchcount ; j++)
	{
	    lump = texture->patches[j].patch;
	    texturememory += lumpinfo[lump].size;
	    W_CachePatchNum(lump , PU_CACHE);
	}
    }

    Z_Free(texturepresent);
    
    // Precache sprites.
    spritepresent = Z_Malloc(numsprites, PU_STATIC, NULL);
    memset (spritepresent,0, numsprites);
	
    for (th = thinkercap.next ; th != &thinkercap ; th=th->next)
    {
	if (th->function.acp1 == (actionf_p1)P_MobjThinker)
	    spritepresent[((mobj_t *)th)->sprite] = 1;
    }
	
    spritememory = 0;
    for (i=0 ; i<numsprites ; i++)
    {
	if (!spritepresent[i])
	    continue;

	for (j=0 ; j<sprites[i].numframes ; j++)
	{
	    sf = &sprites[i].spriteframes[j];
	    for (k=0 ; k<8 ; k++)
	    {
		lump = firstspritelump + sf->lump[k];
		spritememory += lumpinfo[lump].size;
		W_CachePatchNum(lump , PU_CACHE);
	    }
	}
    }

    Z_Free(spritepresent);
}
