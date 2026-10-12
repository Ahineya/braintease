/* Exercise Doom's actual WAD directory, hash lookup, raw cache and zone code.
 * The shareware directory crosses the final partial 64 KiB storage block;
 * the VM disk copy must be block-padded while DOOM_WAD_LENGTH stays logical.
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "doom_build_config.h"
#include "doom_decode.h"
#include "i_system.h"
#include "w_wad.h"
#include "z_zone.h"

void I_Error(char *format, ...)
{
    va_list args;
    va_start(args, format);
    printf("WAD LOOKUP ERROR ");
    vprintf(format, args);
    va_end(args);
    putchar('\n');
    exit(1);
}

byte *I_ZoneBase(size_t *size)
{
    *size = 1024UL * 1024UL;
    return malloc(*size);
}

void I_BeginRead(void) {}
void I_EndRead(void) {}

/* Only used in W_CheckCorrectIWAD's error text, outside this fixture. */
char *D_SuggestGameName(GameMission_t mission, GameMode_t mode)
{
    (void)mission;
    (void)mode;
    return "fixture";
}
char *D_GameMissionString(GameMission_t mission)
{
    (void)mission;
    return "fixture";
}

static void check(int condition, char *message)
{
    if (!condition) I_Error("%s", message);
}

static void check_directory(void)
{
    /* These entries follow the serialized 4 MiB boundary, including the
     * eight-character name that straddles it. Both search paths must work. */
    check(numlumps == 1264, "directory count");
    check(W_CheckNumForName("COMP02_8") == 1156, "split directory name");
    check(W_CheckNumForName("DOOR2_1") == 1170, "final block patch");
    check(W_CheckNumForName("door2_4") == 1171, "case-insensitive patch");
    check(W_CheckNumForName("F_END") == 1263, "last directory entry");
    check(W_CheckNumForName("ABSENT") == -1, "absent name");
}

int main(void)
{
    byte *names, *data;
    uint32_t count, texture_count, names_length, length;
    uint32_t n, j, offset, patch_count, patch_index, references = 0;
    unsigned int missing = 0;
    int lump;
    int *lookup;
    char name[9];

    Z_Init();
    check(W_AddFile(DOOM_WAD_PATH) != NULL, "open configured WAD");
    check_directory();
    W_GenerateHashTable();
    check_directory();

    lump = W_GetNumForName("PNAMES");
    names_length = W_LumpLength(lump);
    names = W_CacheLumpNum(lump, PU_STATIC);
    check(names_length >= 4UL, "PNAMES header");
    count = DoomReadLE32(names);
    check(count == 350UL && count <= (names_length - 4UL) / 8UL,
          "PNAMES count");
    lookup = Z_Malloc((size_t)count * sizeof(*lookup), PU_STATIC, NULL);
    for (n = 0; n < count; ++n) {
        DoomReadName8(names + 4UL + n * 8UL, name);
        lookup[n] = W_CheckNumForName(name);
        if (lookup[n] < 0) ++missing;
    }
    /* Shareware retains unused registered-game names; only texture
     * references require a corresponding patch lump. */
    check(missing == 187, "unused PNAMES entries");
    W_ReleaseLumpNum(lump);

    lump = W_GetNumForName("TEXTURE1");
    length = W_LumpLength(lump);
    data = W_CacheLumpNum(lump, PU_STATIC);
    check(length >= 4UL, "TEXTURE1 header");
    texture_count = DoomReadLE32(data);
    check(texture_count == 125UL && texture_count <= (length - 4UL) / 4UL,
          "texture count");
    for (n = 0; n < texture_count; ++n) {
        offset = DoomReadLE32(data + 4UL + n * 4UL);
        check(offset <= length && length - offset >= 22UL, "texture range");
        patch_count = DoomReadLE16(data + offset + 20UL);
        check(patch_count <= (length - offset - 22UL) / 10UL, "patch range");
        for (j = 0; j < patch_count; ++j) {
            patch_index = DoomReadLE16(data + offset + 26UL + j * 10UL);
            check(patch_index < count && lookup[patch_index] >= 0,
                  "referenced patch lookup");
            ++references;
        }
    }
    check(references == 598UL, "texture reference count");
    W_ReleaseLumpNum(lump);
    Z_Free(lookup);
    Z_CheckHeap();
    puts("WAD LOOKUP OK 1264 lumps, 350 names, 598 references, final block");
    return 0;
}
