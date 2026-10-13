/* Check the compiled-C/leaf-assembly boundary with global and stack pointers.
 * The Rust fixture additionally checks bank crossings, tags and all pixels. */
#include "render_fast.h"
#ifndef RIPPLE
void Doom_ConvertRGB565(unsigned short *dest, const unsigned char *source,
                       const unsigned short *palette, unsigned int count)
{
    while (count--) *dest++ = palette[*source++ & 255U];
}
void Doom_ConvertRGB565Octets(unsigned short *dest, const unsigned char *source,
                             const unsigned short *palette, unsigned int count)
{
    while (count--) *dest++ = palette[*source++];
}
void Doom_CopyPatchColumn(unsigned char *dest, const unsigned char *source,
                          unsigned int count)
{
    while (count--) { *dest = *source++; dest += 320; }
}
void Doom_ConvertRGB565SampledRow(unsigned short *dest, const unsigned char *source,
                                 const unsigned short *palette, unsigned int count,
                                 unsigned int scale)
{
    while (count) { *dest = palette[*source]; if (--count) { ++dest; source += scale; } }
}
void Doom_CopyPatchColumnSampled(unsigned char *dest, const unsigned char *source,
                                 unsigned int count, unsigned int scale)
{
    while (count) { *dest = *source; if (--count) { source += scale; dest += 320 * scale; } }
}
void Doom_CopySamples(unsigned char *dest, const unsigned char *source,
                       unsigned int count, unsigned int scale)
{
    while (count) { *dest = *source; if (--count) { source += scale; dest += scale; } }
}
void Doom_FillSamples(unsigned char *dest, unsigned int value,
                       unsigned int count, unsigned int scale)
{
    while (count) { *dest = value; if (--count) dest += scale; }
}
#endif
static unsigned short global_palette[256];
static unsigned char global_source[19];
static unsigned char global_column[320 * 3];

int main(void)
{
    unsigned short destination[19];
    unsigned char column_source[3];
    unsigned char fill_destination[19];
    unsigned int i;
    for (i = 0; i < 256; ++i) global_palette[i] = i * 257U;
    for (i = 0; i < 19; ++i) global_source[i] = i * 7U;
    Doom_ConvertRGB565(destination, global_source, global_palette, 19);
    for (i = 0; i < 19; ++i)
        if (destination[i] != global_palette[i * 7U]) return 1;
    for (i = 0; i < 19; ++i) destination[i] = 0;
    Doom_ConvertRGB565Octets(destination, global_source, global_palette, 19);
    for (i = 0; i < 19; ++i)
        if (destination[i] != global_palette[i * 7U]) return 3;
    Doom_ConvertRGB565SampledRow(destination, global_source, global_palette, 5, 4);
    for (i = 0; i < 5; ++i)
        if (destination[i] != global_palette[i * 28U]) return 4;
    Doom_CopySamples((unsigned char *)destination, global_source, 5, 4);
    for (i = 0; i < 5; ++i)
        if (((unsigned char *)destination)[i * 4] != global_source[i * 4]) return 5;
    column_source[0] = 7; column_source[1] = 99; column_source[2] = 231;
    Doom_CopyPatchColumn(global_column, column_source, 3);
    for (i = 0; i < 3; ++i)
        if (global_column[i * 320] != column_source[i]) return 2;
    Doom_CopyPatchColumnSampled(global_column, column_source, 2, 2);
    if (global_column[0] != column_source[0] || global_column[640] != column_source[2]) return 6;
    for (i = 0; i < 19; ++i) fill_destination[i] = global_column[i] = 73;
    Doom_FillSamples(fill_destination + 1, 231, 5, 4);
    Doom_FillSamples(global_column + 2, 99, 8, 2);
    for (i = 0; i < 19; ++i) {
        if (fill_destination[i] != (i % 4 == 1 ? 231 : 73)) return 7;
        if (global_column[i] != (i >= 2 && i <= 16 && i % 2 == 0 ? 99 : 73)) return 8;
    }
    Doom_FillSamples((unsigned char *)0, 0, 0, 4);
    return 0;
}
