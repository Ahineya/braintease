/* Check the compiled-C/leaf-assembly boundary with global and stack pointers.
 * The Rust fixture additionally checks bank crossings, tags and all pixels. */
#include "render_fast.h"
#ifndef RIPPLE
void Doom_ConvertRGB565(unsigned short *dest, const unsigned char *source,
                       const unsigned short *palette, unsigned int count)
{
    while (count--) *dest++ = palette[*source++ & 255U];
}
void Doom_CopyPatchColumn(unsigned char *dest, const unsigned char *source,
                          unsigned int count)
{
    while (count--) { *dest = *source++; dest += 320; }
}
#endif
static unsigned short global_palette[256];
static unsigned char global_source[19];
static unsigned char global_column[320 * 3];

int main(void)
{
    unsigned short destination[19];
    unsigned char column_source[3];
    unsigned int i;
    for (i = 0; i < 256; ++i) global_palette[i] = i * 257U;
    for (i = 0; i < 19; ++i) global_source[i] = i * 7U;
    Doom_ConvertRGB565(destination, global_source, global_palette, 19);
    for (i = 0; i < 19; ++i)
        if (destination[i] != global_palette[i * 7U]) return 1;
    column_source[0] = 7; column_source[1] = 99; column_source[2] = 231;
    Doom_CopyPatchColumn(global_column, column_source, 3);
    for (i = 0; i < 3; ++i)
        if (global_column[i * 320] != column_source[i]) return 2;
    return 0;
}
