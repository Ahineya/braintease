/* Compare every final metric against native execution of the original WAD
 * path. The cooked destination starts at bank cell 63999, splitting the very
 * first 32-bit metric across two banks without changing the machine layout. */
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "doom_build_config.h"
#include "r_local.h"
#include "i_system.h"
#include "z_zone.h"
#include "patch_cache.h"
#include "asset_metadata.h"
#include "deh_main.h"

int firstspritelump, lastspritelump, numspritelumps;
fixed_t *spritewidth, *spriteoffset, *spritetopoffset;
boolean modifiedgame;
static unsigned char *allocations[3], *destinations[3];
static size_t lengths[3];
static int allocation_count;

void I_Error(char *format, ...)
{
    va_list args;
    va_start(args, format);
    printf("SPRITE METRICS ERROR "); vprintf(format, args); va_end(args);
    putchar('\n'); exit(1);
}
byte *I_ZoneBase(size_t *size) { *size = 2UL * 1024UL * 1024UL; return malloc(*size); }
void I_BeginRead(void) {}
void I_EndRead(void) {}
char *D_SuggestGameName(GameMission_t mission, GameMode_t mode)
{ (void)mission; (void)mode; return "fixture"; }
char *D_GameMissionString(GameMission_t mission) { (void)mission; return "fixture"; }

static void *metrics_malloc(size_t size, int tag, void *user)
{
    unsigned char *allocation, *dest;
    int n = allocation_count++;
    (void)tag; (void)user;
    if (n >= 3 || size > 200000UL) I_Error("metric allocation size");
    allocation = malloc(size + 128002UL);
    if (allocation == NULL) I_Error("metric allocation failed");
#ifdef RIPPLE
    dest = (unsigned char *)(((((uintptr_t)allocation >> 16) + 1UL) << 16) | 63999UL);
#else
    dest = allocation + 128000UL;
#endif
    allocations[n] = allocation; destinations[n] = dest; lengths[n] = size;
    dest[-1] = 93; dest[size] = 167;
    return dest;
}
#define Z_Malloc metrics_malloc
#include "sprite_metrics.inc"
#undef Z_Malloc

int main(void)
{
    int i;
    Z_Init();
    if (W_AddFile(DOOM_WAD_PATH) == NULL) I_Error("open WAD");
    W_GenerateHashTable();
    R_InitSpriteLumps();
    Z_CheckHeap();
    for (i = 0; i < allocation_count; ++i)
        if (destinations[i][-1] != 93 || destinations[i][lengths[i]] != 167)
            I_Error("metric destination guard");
#if defined(RIPPLE) && DOOM_WAD_MODE == 2
    if (numspritelumps ? allocation_count != 1 : allocation_count != 0)
        I_Error("metrics did not use one slab");
    if (!numspritelumps && (spritewidth != NULL || spriteoffset != NULL || spritetopoffset != NULL))
        I_Error("empty metrics are not null");
    if (numspritelumps && (spriteoffset != spritewidth + numspritelumps
        || spritetopoffset != spriteoffset + numspritelumps))
        I_Error("metric arrays are not contiguous");
#endif
    printf("SPRITE METRICS INITIALIZED %d %d %d\n", firstspritelump, lastspritelump, numspritelumps);
    for (i = 0; i < numspritelumps; ++i)
        printf("METRIC %d %ld %ld %ld\n", i, (long)spritewidth[i], (long)spriteoffset[i], (long)spritetopoffset[i]);
    puts("SPRITE METRICS OK");
    for (i = 0; i < allocation_count; ++i) free(allocations[i]);
    return 0;
}
