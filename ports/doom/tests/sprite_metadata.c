/* Native consumes original WAD names through the production resolver; Ripple
 * consumes prepared tables. The validator compares every emitted frame cell. */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "doom_build_config.h"
#include "sprite_metadata.inc"
#include "sprite_names.inc"

int firstspritelump, lastspritelump, numspritelumps;
boolean modifiedgame;

void I_Error(char *format, ...)
{
    va_list args;
    va_start(args, format);
    printf("SPRITE METADATA ERROR ");
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

int main(void)
{
    int sprite, frame, rotation;
    unsigned long total = 0;
    Z_Init();
    if (W_AddFile(DOOM_WAD_PATH) == NULL) I_Error("open WAD");
    W_GenerateHashTable();
    firstspritelump = W_GetNumForName("S_START") + 1;
    lastspritelump = W_GetNumForName("S_END") - 1;
    numspritelumps = lastspritelump - firstspritelump + 1;
    sprtemp = Z_Malloc(29U * sizeof(*sprtemp), PU_STATIC, NULL);
    R_InitSpriteDefs(sprnames);
    Z_CheckHeap();
    puts("SPRITE METADATA INITIALIZED");
    for (sprite = 0; sprite < numsprites; ++sprite)
    {
        spritedef_t *definition = sprites + sprite;
#if defined(RIPPLE) && DOOM_WAD_MODE == 2
        if (!definition->numframes && definition->spriteframes != NULL)
            I_Error("noncanonical absent-sprite pointer");
#endif
        if (definition->numframes)
            printf("SPRITE %d %d\n", sprite, definition->numframes);
        for (frame = 0; frame < definition->numframes; ++frame)
        {
            spriteframe_t *entry = definition->spriteframes + frame;
            printf("FRAME %d %d %d", sprite, frame, (int)entry->rotate);
            for (rotation = 0; rotation < 8; ++rotation)
                printf(" %d", entry->lump[rotation]);
            for (rotation = 0; rotation < 8; ++rotation)
                printf(" %d", (int)entry->flip[rotation]);
            putchar('\n');
            ++total;
        }
    }
    printf("SPRITE METADATA OK %d sprites, %lu frames\n", numsprites, total);
    return 0;
}
