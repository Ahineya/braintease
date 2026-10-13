/* Native oracle fixture: execute the same drawing operations at each scale.
 * The driver compares retained cells to scale 1 and checks skipped cells were
 * never touched. Geometry deliberately crosses every sampling-grid phase. */
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include "doomtype.h"
#include "i_video.h"
#include "v_video.h"
#include "doom_resolution.h"

byte *I_VideoBuffer;
extern byte *xlatab;
void F_DrawPatchCol(int x, patch_t *patch, int col);

static byte screen[320 * 200];
static byte backing[320 * 200];
static byte raw[320 * 200];
static byte tint[65536], xla[65536];
static FILE *output;

void I_Error(char *format, ...)
{
    va_list args;
    va_start(args, format);
    vfprintf(stderr, format, args);
    va_end(args);
    fputc('\n', stderr);
    exit(89);
}
void M_AddToBox(fixed_t *box, fixed_t x, fixed_t y) { (void)box; (void)x; (void)y; }

static byte initial(size_t i) { return (byte)(31 + i * 13 + i / 320); }
static void reset(void)
{
    size_t i;
    for (i = 0; i < sizeof(screen); ++i)
        screen[i] = initial(i);
    V_RestoreBuffer();
}
static void record(void)
{
    size_t i;
    for (i = 0; i < sizeof(screen); ++i)
        if (((i % 320) % DOOM_RESOLUTION_SCALE || (i / 320) % DOOM_RESOLUTION_SCALE)
         && screen[i] != initial(i))
            I_Error("Discarded pixel changed at %lu", (unsigned long)i);
    if (fwrite(screen, 1, sizeof(screen), output) != sizeof(screen))
        I_Error("Cannot write fixture output");
}

static patch_t *make_patch(void)
{
    byte *data = calloc(1, 2048);
    patch_t *patch = (patch_t *)data;
    unsigned col, row, offset = 8 + 9 * 4;
    patch->width = 9;
    patch->height = 15;
    patch->leftoffset = 2;
    patch->topoffset = 3;
    for (col = 0; col < 9; ++col)
    {
        patch->columnofs[col] = offset;
        data[offset] = col % 3;
        data[offset + 1] = 5;
        for (row = 0; row < 5; ++row)
            data[offset + 3 + row] = (byte)(17 + col * 7 + row * 11);
        offset += 9;
        data[offset] = 9 + col % 2;
        data[offset + 1] = 4;
        for (row = 0; row < 4; ++row)
            data[offset + 3 + row] = (byte)(113 + col * 3 + row * 5);
        offset += 8;
        data[offset++] = 255;
    }
    return patch;
}

int main(int argc, char **argv)
{
    patch_t *patch = make_patch();
    size_t i;
    int x, y, operation;
    I_VideoBuffer = screen;
    tinttable = tint;
    xlatab = xla;
    for (i = 0; i < sizeof(raw); ++i) raw[i] = (byte)(i * 23 + i / 320);
    for (i = 0; i < sizeof(tint); ++i)
    {
        tint[i] = (byte)((i >> 8) * 3 + (i & 255) * 5);
        xla[i] = (byte)((i >> 8) * 7 + (i & 255) * 11);
    }
    reset();
    if (argc == 3 && !strcmp(argv[2], "unaligned"))
    {
        V_CopyRect(0, 0, backing, 4, 4, 1, 1);
        return 0;
    }
    if (argc != 2 || !(output = fopen(argv[1], "wb"))) return 2;
    for (operation = 0; operation < 6; ++operation)
        for (x = 0; x < 4; ++x)
            for (y = 0; y < 4; ++y)
            {
                reset();
                switch (operation)
                {
                    case 0: V_DrawPatch(10 + x, 10 + y, patch); break;
                    case 1: V_DrawPatchFlipped(10 + x, 10 + y, patch); break;
                    case 2: V_DrawTLPatch(10 + x, 10 + y, patch); break;
                    case 3: V_DrawAltTLPatch(10 + x, 10 + y, patch); break;
                    case 4: V_DrawXlaPatch(10 + x, 10 + y, patch); break;
                    case 5: V_DrawShadowedPatch(10 + x, 10 + y, patch); break;
                }
                record();
            }
    for (x = 0; x < 4; ++x)
        for (y = 0; y < 4; ++y)
        {
            reset(); V_DrawBlock(x, y, 17, 19, raw); record();
            reset(); V_DrawFilledBox(x, y, 17, 19, 67); record();
            reset(); V_DrawBox(x, y, 17, 19, 113); record();
            // Backing stores use the same phase; status bar moves down 168 rows.
            reset();
            memset(backing, 0, sizeof(backing));
            V_UseBuffer(backing);
            V_DrawPatch(10 + x, 10 + y, patch);
            V_RestoreBuffer();
            V_CopyRect(x, y, backing, 24, 28, x, y + 168);
            record();
        }
    reset(); V_DrawRawScreen(raw); record();
    // Right and bottom boundaries, zero extents and shorter-than-scale ranges.
    reset(); V_DrawBlock(319, 199, 1, 1, raw); record();
    reset(); V_DrawFilledBox(317, 197, 3, 3, 79); record();
    reset(); V_DrawBlock(320, 200, 0, 0, raw); record();
    reset(); V_DrawPatch(313, 188, patch); record();
    reset();
    for (x = 0; x < 9; ++x) F_DrawPatchCol(17 + x, patch, 8 - x);
    record();
    free(patch);
    if (fclose(output)) return 2;
    return 0;
}
