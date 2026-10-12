/* Native regression checks for engine world math and the packed span drawer.
 * Uses the actual patched functions. Link unused gameplay/rendering sections
 * away, as in map_decode.c, adding these patched engine sources:
 *   p_maputl.c m_bbox.c m_fixed.c
 * r_draw.c is included below to exercise its private background buffer too.
 * See map_decode.c for include paths, compiler and dead-section flags.
 */
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "r_draw.c"
#include "m_bbox.h"
#include "p_local.h"

byte *I_VideoBuffer;
int centery;

void I_Error(char *format, ...)
{
    va_list args;
    va_start(args, format);
    vfprintf(stderr, format, args);
    va_end(args);
    fputc('\n', stderr);
    abort();
}

static void TestWorldMath(void)
{
    fixed_t box[4];
    vertex_t vertex;
    line_t line;
    memset(&vertex, 0, sizeof(vertex));
    memset(&line, 0, sizeof(line));
    M_ClearBox(box);
    assert(box[BOXTOP] == INT32_MIN && box[BOXLEFT] == INT32_MAX);
    M_AddToBox(box, -300 * FRACUNIT, -700 * FRACUNIT);
    M_AddToBox(box, 1200 * FRACUNIT, 900 * FRACUNIT);
    assert(box[BOXLEFT] == -300 * FRACUNIT);
    assert(box[BOXRIGHT] == 1200 * FRACUNIT);
    assert(box[BOXBOTTOM] == -700 * FRACUNIT);
    assert(box[BOXTOP] == 900 * FRACUNIT);
    assert(P_AproxDistance(-300 * FRACUNIT, 400 * FRACUNIT)
           == 550 * FRACUNIT);
    line.v1 = &vertex;
    line.dx = 128 * FRACUNIT;
    line.dy = 64 * FRACUNIT;
    assert(P_PointOnLineSide(64 * FRACUNIT, 48 * FRACUNIT, &line) == 1);
    assert(P_PointOnLineSide(64 * FRACUNIT, 16 * FRACUNIT, &line) == 0);
}

static void TestFramebuffer(void)
{
    byte texture[4096];
    byte column[128];
    byte palette[256];
    size_t pixels = (size_t) SCREENWIDTH * SCREENHEIGHT;
    size_t i;
    I_VideoBuffer = calloc(pixels, sizeof(*I_VideoBuffer));
    assert(I_VideoBuffer != NULL && pixels == 64000);
    for (i = 0; i < 4096; ++i) texture[i] = (byte) (i % 251);
    for (i = 0; i < 128; ++i) column[i] = (byte) i;
    for (i = 0; i < 256; ++i) palette[i] = (byte) i;
    R_InitBuffer(SCREENWIDTH, SCREENHEIGHT);
    assert(ylookup[199] == I_VideoBuffer + 63680);
    {
        byte **saved_ylookup = ylookup;
        int *saved_columnofs = columnofs;
        assert(ylookup[MAXHEIGHT - 1] == NULL && columnofs[MAXWIDTH - 1] == 0);
        ylookup[MAXHEIGHT - 1] = I_VideoBuffer;
        columnofs[MAXWIDTH - 1] = 701;
        R_InitBuffer(160, 100);
        assert(ylookup == saved_ylookup && columnofs == saved_columnofs);
        assert(ylookup[MAXHEIGHT - 1] == I_VideoBuffer && columnofs[MAXWIDTH - 1] == 701);
        assert(columnofs[159] == 239 && ylookup[99] == I_VideoBuffer + 133 * 320);
        R_InitBuffer(SCREENWIDTH, SCREENHEIGHT);
        assert(ylookup[199] == I_VideoBuffer + 63680);
    }

    /* A floor span crosses a texture wrap and writes beyond cell 32767. */
    ds_y = 199;
    ds_x1 = 20;
    ds_x2 = 23;
    ds_xfrac = 62 * FRACUNIT;
    ds_yfrac = 63 * FRACUNIT;
    ds_xstep = FRACUNIT;
    ds_ystep = 0;
    ds_source = texture;
    ds_colormap = palette;
    R_DrawSpan();
    for (i = 0; i < 4; ++i)
        assert(I_VideoBuffer[63700 + i] == texture[4032 + ((62 + i) & 63)]);

    /* The blocky drawer follows the same modulo-32 coordinate rules. */
    ds_y = 198;
    ds_x1 = 10;
    ds_x2 = 11;
    ds_xfrac = -FRACUNIT;
    ds_xstep = FRACUNIT;
    R_DrawSpanLow();
    assert(I_VideoBuffer[63380] == texture[4095]);
    assert(I_VideoBuffer[63381] == texture[4095]);
    assert(I_VideoBuffer[63382] == texture[4032]);
    assert(I_VideoBuffer[63383] == texture[4032]);

    dc_x = 319;
    dc_yl = 198;
    dc_yh = 199;
    dc_iscale = FRACUNIT;
    dc_texturemid = -FRACUNIT;
    centery = 198;
    dc_source = column;
    dc_colormap = palette;
    R_DrawColumn();
    assert(I_VideoBuffer[63679] == 127 && I_VideoBuffer[63999] == 0);

    background_buffer = calloc(pixels, sizeof(*background_buffer));
    assert(background_buffer != NULL);
    background_buffer[50000] = 17;
    background_buffer[50001] = 29;
    R_VideoErase(50000, 2);
    assert(I_VideoBuffer[50000] == 17 && I_VideoBuffer[50001] == 29);
    free(background_buffer);
    background_buffer = NULL;
    free(I_VideoBuffer);
    I_VideoBuffer = NULL;
    free(ylookup);
    free(columnofs);
    ylookup = NULL;
    columnofs = NULL;
}

int main(void)
{
    TestWorldMath();
    TestFramebuffer();
    puts("world widths: fixed geometry, texture wrapping and 64000-cell framebuffer passed");
    return 0;
}
