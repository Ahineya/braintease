/* Exercise the actual C translated/fuzz drawers through RCC and the Rust VM.
 * The reference walks logical pixels independently and checks the complete
 * sparse screen, guards, neighbor-row semantics and fuzz table cursor. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "r_local.h"
#include "doom_resolution.h"

#if DOOM_RESOLUTION_SCALE != 2 && DOOM_RESOLUTION_SCALE != 4
#error sampled_raster requires RESOLUTION_SCALE=2 or 4
#endif

byte *I_VideoBuffer;
lighttable_t *colormaps;
int centery;
GameMode_t gamemode;
extern int fuzzpos;

static byte *pixels, *expected;
static byte source[256], colors[256], translation[256];
static const int directions[50] = {
    1,-1,1,-1,1,1,-1,1,1,-1,1,1,1,-1,1,1,1,-1,-1,-1,-1,
    1,-1,-1,1,1,1,1,-1,1,-1,1,1,-1,-1,1,1,-1,-1,-1,-1,1,1,
    1,1,-1,1,1,-1,1
};

void I_Error(char *message, ...) { printf("SAMPLED RASTER ERROR %s\n", message); exit(1); }
void *Z_Malloc(size_t size, int tag, void *user) { return malloc(size); }
void Z_Free(void *memory) { free(memory); }
void *W_CacheLumpName(char *name, int tag) { I_Error("unexpected lump access"); return NULL; }
patch_t *W_CachePatchName(char *name, int tag) { I_Error("unexpected patch access"); return NULL; }
void V_UseBuffer(byte *buffer) { I_Error("unexpected buffer switch"); }
void V_RestoreBuffer(void) { I_Error("unexpected buffer restore"); }
void V_DrawPatch(int x, int y, patch_t *patch) { I_Error("unexpected patch draw"); }
void V_MarkRect(int x, int y, int width, int height) { I_Error("unexpected dirty rectangle"); }

static void reset(void)
{
    unsigned int x, y;
    memset(pixels, 213, 64000UL);
    for (y = 0; y < 200; y += DOOM_RESOLUTION_SCALE)
        for (x = 0; x < 320; x += DOOM_RESOLUTION_SCALE)
            pixels[(uint32_t)y * 320U + x] = (x * 7U + y * 13U) & 255U;
    memcpy(expected, pixels, 64000UL);
    pixels[-1] = 19;
    pixels[64000UL] = 71;
}

static void check(unsigned int case_number, int whole_screen)
{
    uint32_t i;
    unsigned int x, y;
    if (pixels[-1] != 19 || pixels[64000UL] != 71)
        I_Error("screen guard overwritten");
    /* All draws use the first eight physical columns of the view. Check that
     * entire strip after each call (including discarded rows and columns),
     * and the rest of the screen after each view. This keeps test bookkeeping
     * from dwarfing the actual RCC drawer execution. */
    for (y = 0; y < (whole_screen ? 200 : viewheight); ++y)
        for (x = 0; x < (whole_screen ? 320 : 8); ++x)
    {
        i = whole_screen ? (uint32_t)y * 320U + x
          : (uint32_t)(y + viewwindowy) * 320U + x + viewwindowx;
        if (pixels[i] != expected[i])
        {
            printf("SAMPLED RASTER mismatch case=%u cell=%lu got=%u expected=%u\n",
                   case_number, (unsigned long)i, pixels[i], expected[i]);
            exit(1);
        }
    }
}

static void translated(int low)
{
    int y, x = viewwindowx + dc_x * (low ? 2 : 1);
    uint32_t frac = (uint32_t)dc_texturemid
                  + (uint32_t)(dc_yl - centery) * (uint32_t)dc_iscale;
    for (y = dc_yl; y <= dc_yh; ++y)
    {
        if (x % DOOM_RESOLUTION_SCALE == 0
         && (y + viewwindowy) % DOOM_RESOLUTION_SCALE == 0)
            expected[(uint32_t)(y + viewwindowy) * 320U + x]
                = colors[translation[source[frac >> 16]]];
        frac += (uint32_t)dc_iscale;
    }
    if (low) R_DrawTranslatedColumnLow(); else R_DrawTranslatedColumn();
}

static void fuzz(int low)
{
    int y, cursor = fuzzpos;
    int x = viewwindowx + dc_x * (low ? 2 : 1);
    for (y = dc_yl; y <= dc_yh; ++y)
    {
        if (y >= DOOM_RESOLUTION_SCALE && y + DOOM_RESOLUTION_SCALE < viewheight
         && x % DOOM_RESOLUTION_SCALE == 0
         && (y + viewwindowy) % DOOM_RESOLUTION_SCALE == 0)
        {
            uint32_t cell = (uint32_t)(y + viewwindowy) * 320U + x;
            int32_t neighbor = (int32_t)cell + directions[cursor] * (320 * DOOM_RESOLUTION_SCALE);
            expected[cell] = colormaps[6 * 256 + expected[neighbor]];
            cursor = (cursor + 1) % 50;
        }
    }
    if (low) R_DrawFuzzColumnLow(); else R_DrawFuzzColumn();
    if (fuzzpos != cursor) I_Error("fuzz cursor mismatch");
}

int main(void)
{
    unsigned int i, cases = 0;
    int window, low, phase, x;
    byte *allocation = malloc(64002UL);
    expected = malloc(64000UL);
    colormaps = malloc(8192);
    if (!allocation || !expected || !colormaps) return 1;
    pixels = I_VideoBuffer = allocation + 1;
    for (i = 0; i < 256; ++i)
    {
        source[i] = (i * 23U + 7U) & 255U;
        colors[i] = (i * 31U + 19U) & 255U;
        translation[i] = (255U - i);
    }
    for (i = 0; i < 8192; ++i) colormaps[i] = (i * 17U + i / 256U) & 255U;
    dc_source = source; dc_colormap = colors; dc_translation = translation;
    for (window = 0; window < 2; ++window)
    {
        R_InitBuffer(window ? 96 : 320, window ? 48 : 200);
        viewheight = window ? 48 : 200;
        centery = 24;
        reset();
        for (low = 0; low < 2; ++low)
            for (phase = 0; phase < 4; ++phase)
                for (x = 0; x < 4; ++x)
                {
                    dc_x = x;
                    dc_yl = phase; dc_yh = 31 + phase;
                    dc_texturemid = 48UL * 65536UL + 0xf100UL;
                    dc_iscale = 0x0000fff1UL;
                    translated(low); check(++cases, 0);
                    dc_yh = viewheight - 1 - phase;
                    fuzzpos = phase ? 49 : 0;
                    fuzz(low); check(++cases, 0);
                }
        check(cases, 1);
    }
    printf("SAMPLED RASTER OK scale %u, %u translated/fuzz cases, sparse cells, neighbors, bounds and cursor\n",
           DOOM_RESOLUTION_SCALE, cases);
    return 0;
}
