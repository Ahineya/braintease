/* Compare the actual renderer initialization against the original IWAD.
 * This oracle deliberately does not consume the packer's parsed objects.
 * The same checks run on the native/raw and Ripple/cooked paths. */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "doom_build_config.h"
#include "texture_metadata.inc"

void I_Error(char *format, ...)
{
    va_list args;
    va_start(args, format);
    printf("TEXTURE METADATA ERROR ");
    vprintf(format, args);
    va_end(args);
    putchar('\n');
    exit(1);
}

byte *I_ZoneBase(size_t *size)
{
    *size = 2UL * 1024UL * 1024UL;
    return malloc(*size);
}
void I_BeginRead(void) {}
void I_EndRead(void) {}
char *D_SuggestGameName(GameMission_t mission, GameMode_t mode)
{
    (void)mission; (void)mode;
    return "fixture";
}
char *D_GameMissionString(GameMission_t mission)
{
    (void)mission;
    return "fixture";
}
static void check(int good, char *message)
{
    if (!good) I_Error("%s", message);
}

/* Reconstruct each column independently, starting from the source patch
 * records. This checks clipped/overlapping patches, final patch precedence,
 * actual decoded-patch offsets, and short texture sampling windows. */
static void check_columns(int index)
{
    texture_t *texture = textures[index];
    size_t composite_size = 0;
    size_t pitch = texture->height < 128 ? 128 : texture->height;
    int x, p;
    for (x = 0; x < texture->width; ++x) {
        int coverage = 0;
        int lump = -1;
        uint32_t offset = 0;
        for (p = 0; p < texture->patchcount; ++p) {
            texpatch_t *record = &texture->patches[p];
            patch_t *patch = W_CachePatchHeaderNum(record->patch, PU_CACHE);
            int32_t relative = (int32_t)x - record->originx;
            if (relative >= 0 && relative < patch->width) {
                ++coverage;
                lump = record->patch;
                offset = (uint32_t)patch->columnofs[relative] + 3UL;
            }
        }
        check(coverage != 0, "reference column has no patch");
        if (coverage > 1) {
            lump = -1;
            offset = (uint32_t)composite_size;
            composite_size += pitch;
        }
        check(texturecolumnlump[index][x] == lump, "column patch mapping");
        check(texturecolumnofs[index][x] == offset, "column offset mapping");
    }
    check(texturecompositesize[index] == composite_size, "composite size");
}

static void check_hash(void)
{
    int i, j, bucket;
    for (i = 0; i < numtextures; ++i) {
        texture_t *entry;
        int visited = 0;
        int first = i;
        for (j = 0; j < i; ++j) {
            if (!strncasecmp(textures[j]->name, textures[i]->name, 8)) {
                first = j;
                break;
            }
        }
        bucket = W_LumpNameHash(textures[i]->name) % numtextures;
        entry = textures_hashtable[bucket];
        while (entry && strncasecmp(entry->name, textures[i]->name, 8)) {
            check(++visited <= numtextures, "texture hash cycle");
            entry = entry->next;
        }
        check(entry == textures[first], "first duplicate texture wins");
    }
}

int main(void)
{
    byte *names;
    uint32_t pnames_count, refs = 0, columns = 0;
    int pass, index = 0;
    int texture_lumps[2];
    Z_Init();
    check(W_AddFile(DOOM_WAD_PATH) != NULL, "open WAD");
    W_GenerateHashTable();
    R_InitTextures();
    Z_CheckHeap();
    puts("TEXTURE METADATA INITIALIZED");
    names = W_CacheLumpName("PNAMES", PU_STATIC);
    pnames_count = DoomReadLE32(names);
    texture_lumps[0] = W_GetNumForName("TEXTURE1");
    texture_lumps[1] = W_CheckNumForName("TEXTURE2");
    for (pass = 0; pass < 2; ++pass) {
        byte *data;
        uint32_t count, n;
        if (texture_lumps[pass] < 0) continue;
        data = W_CacheLumpNum(texture_lumps[pass], PU_STATIC);
        count = DoomReadLE32(data);
        for (n = 0; n < count; ++n, ++index) {
            uint32_t offset = DoomReadLE32(data + 4UL + n * 4UL);
            byte *record = data + offset;
            unsigned int pcount = DoomReadLE16(record + 20);
            texture_t *texture;
            int mask = 1;
            unsigned int p;
            check(index < numtextures, "texture count");
            texture = textures[index];
            check(!memcmp(texture->name, record, 8), "texture name");
            check(texture->width == (short)DoomReadLE16(record + 12)
                && texture->height == (short)DoomReadLE16(record + 14), "texture dimensions");
            check(texture->index == index && texture->patchcount == pcount, "texture index/patch count");
            check(texturetranslation[index] == index, "identity texture translation");
            check(texturecomposite[index] == NULL, "canonical null composite pointer");
            while (mask * 2L <= texture->width) mask *= 2;
            check(texturewidthmask[index] == mask - 1, "texture width mask");
            check(textureheight[index] == (fixed_t)texture->height * FRACUNIT, "fixed texture height");
            for (p = 0; p < pcount; ++p) {
                byte *part = record + 22UL + (uint32_t)p * 10UL;
                unsigned int name_index = DoomReadLE16(part + 4);
                char name[9];
                check(name_index < pnames_count, "source PNAMES index");
                DoomReadName8(names + 4UL + (uint32_t)name_index * 8UL, name);
                check(texture->patches[p].patch == W_GetNumForName(name), "resolved patch ID");
                check(texture->patches[p].originx == (short)DoomReadS16(part)
                    && texture->patches[p].originy == (short)DoomReadS16(part + 2), "signed patch origin");
                ++refs;
            }
            columns += texture->width;
            check_columns(index);
        }
        W_ReleaseLumpNum(texture_lumps[pass]);
    }
    check(index == numtextures, "all source textures loaded");
    W_ReleaseLumpName("PNAMES");
    check_hash();
    Z_CheckHeap();
    printf("TEXTURE METADATA OK %d textures, %lu references, %lu columns\n",
           numtextures, (unsigned long)refs, (unsigned long)columns);
    return 0;
}
