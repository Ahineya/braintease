"""Remove host object-representation assumptions from LGeneral's SDL drawing.

SDL pixels are logical octets, including on C16 where a C char occupies a cell.
Packing a Uint32 with memcpy or aliasing its address consequently gives a
different result on C16.  Explicit shifts work with 16-, 24- and 32-bit surfaces
and leave the game rules and original raster artwork unchanged.
"""

from pathlib import Path
import re


def _replace(text: str, old: str, new: str, name: str) -> str:
    count = text.count(old)
    if count != 1:
        raise ValueError(f"LGeneral source changed at {name}: expected one match, found {count}")
    return text.replace(old, new, 1)


def adapt(source_root: Path, c16: bool) -> None:
    """Adapt a freshly extracted source tree, before the optional C16 ABI pass."""
    del c16  # Logical-octet surface storage is also used by the host adapter.
    source_root = Path(source_root)
    path = source_root / "src/lg-sdl.c"
    text = path.read_text()
    start = text.index("/* set pixel */")
    end = text.index("/* sdl font */", start)
    text = text[:start] + """/* set pixel: pixels/pitch are logical octets, not native scalar bytes. */
Uint32 set_pixel( SDL_Surface *surf, int x, int y, int pixel )
{
    unsigned char *pixels = (unsigned char *)surf->pixels;
    Uint32 pos, value = (Uint32)pixel;
    int i, bytes = surf->format->BytesPerPixel;
    if (x < 0 || y < 0 || x >= surf->w || y >= surf->h)
        return value;
    pos = (Uint32)y * surf->pitch + (Uint32)x * bytes;
    for (i = 0; i < bytes; i++)
        pixels[pos + i] = (unsigned char)((value >> (i * 8)) & 255);
    return value;
}

/* get pixel */
Uint32 get_pixel( SDL_Surface *surf, int x, int y )
{
    const unsigned char *pixels = (const unsigned char *)surf->pixels;
    Uint32 pos, value = 0;
    int i, bytes = surf->format->BytesPerPixel;
    if (x < 0 || y < 0 || x >= surf->w || y >= surf->h)
        return 0;
    pos = (Uint32)y * surf->pitch + (Uint32)x * bytes;
    for (i = 0; i < bytes; i++)
        value |= ((Uint32)pixels[pos + i] & 255) << (i * 8);
    return value;
}

""" + text[end:]
    text, count = re.subn(
        r"#if SDL_BYTEORDER == SDL_BIG_ENDIAN\n.*?#define AT_RGB\(pixel, idx\) .*?\n",
        "", text, count=1, flags=re.DOTALL,
    )
    if count != 1:
        raise ValueError("LGeneral font byte-alias macro changed")
    text = _replace(text, "#undef R_INDEX\n#undef G_INDEX\n#undef B_INDEX\n#undef AT_RGB\n", "", "font byte-alias cleanup")
    text = _replace(text, "            Uint32 pixel = 0;\n            int valid_glyph;",
                    "            Uint32 pixel = 0;\n            Uint8 red, green, blue;\n            int valid_glyph;",
                    "font RGB locals")
    text = _replace(
        text,
        "            SDL_GetRGB(get_pixel(new_glyphs, i, new_glyphs->h - 1), new_glyphs->format, AT_RGB(pixel, R_INDEX), AT_RGB(pixel, G_INDEX), AT_RGB(pixel, B_INDEX));",
        "            SDL_GetRGB(get_pixel(new_glyphs, i, new_glyphs->h - 1), new_glyphs->format, &red, &green, &blue);\n"
        "            pixel = ((Uint32)red << 16) | ((Uint32)green << 8) | blue;",
        "font marker RGB packing",
    )
    text = _replace(text, "            if (code > 256) {", "            if (code >= 256) {", "font glyph bound")
    text = _replace(text,
                    "        for (i = code; i < 256; i++)\n            font->char_offset[code] += fixup;",
                    "        for (i = code; i <= 256; i++)\n            font->char_offset[i] += fixup;",
                    "font following offsets and terminal sentinel")
    path.write_text(text)

    path = source_root / "src/lg-sdl.h"
    text = path.read_text()
    text = _replace(text,
                    "    int         char_offset[256];\n    int\t\twidth;\t// total width of surface, *must* follow char_offset\n    \t\t\t// because it is used for spilling char_offset",
                    "    int         char_offset[257]; /* 256 glyphs plus the final end offset. */\n    int         width; /* total width of surface */",
                    "font terminal offset storage")
    path.write_text(text)

    path = source_root / "src/unit_lib.c"
    text = path.read_text()
    text = _replace(text, "int unit_lib_load( char *fname, int main )", """/* Surface pixels are logical octets. Copies avoid packing a pixel twice. */
static void lg_mirror_right_half(SDL_Surface *surface, int width, int height)
{
    unsigned char *base = (unsigned char *)surface->pixels;
    int x, y, k, bytes = surface->format->BytesPerPixel;
    for (y = 0; y < height; y++) {
        unsigned char *row = base + (Uint32)y * surface->pitch;
        const unsigned char *source = row + (Uint32)(width - 1) * bytes;
        unsigned char *destination = row + (Uint32)width * bytes;
        for (x = 0; x < width; x++) {
            for (k = 0; k < bytes; k++) destination[k] = source[k];
            destination += bytes;
            if (x + 1 < width) source -= bytes;
        }
    }
}

static void lg_scale_two_thirds(SDL_Surface *source, SDL_Surface *destination)
{
    const unsigned char *base = (const unsigned char *)source->pixels;
    unsigned char *output = (unsigned char *)destination->pixels;
    int x, y, k, bytes = source->format->BytesPerPixel;
    for (y = 0; y < destination->h; y++) {
        const unsigned char *row = base + (Uint32)(y + y / 2) * source->pitch;
        unsigned char *target = output + (Uint32)y * destination->pitch;
        for (x = 0; x < destination->w; x++) {
            const unsigned char *pixel = row + (Uint32)(x + x / 2) * bytes;
            for (k = 0; k < bytes; k++) target[k] = pixel[k];
            target += bytes;
        }
    }
}

static void lg_unit_phase(const char *stage, Uint32 *start)
{
    Uint32 now = SDL_GetTicks();
    if (getenv("LGENERAL_TRACE"))
        fprintf(stderr, "LGENERAL UNIT_TIMING %s %lu ms\\n", stage, (unsigned long)(now - *start));
    *start = now;
}

int unit_lib_load( char *fname, int main )""", "logical-octet sprite helpers")
    text = _replace(text, "    int i, j, icon_id;", "    int i, j, icon_id;\n    Uint32 lg_phase_start = SDL_GetTicks();", "unit load timer")
    text = _replace(text, "    domain = determine_domain(pd, fname);", "    lg_unit_phase(\"parse\", &lg_phase_start);\n    domain = determine_domain(pd, fname);", "unit parser phase")
    text = _replace(text, "    /* unit lib entries */", "    lg_unit_phase(\"definitions-and-icons\", &lg_phase_start);\n    /* unit lib entries */", "unit icon phase")
    text = _replace(text, "    parser_free( &pd );\n    /* LOG */", "    lg_unit_phase(\"sprites\", &lg_phase_start);\n    parser_free( &pd );\n    /* LOG */", "unit sprite phase")
    text = _replace(text, "    int byte_size, y_offset;\n    char *pix_buffer;\n", "", "sprite temporary declarations")
    start = text.index("            /* get format info */")
    end = text.index("        }\n        scale = 1.5;", start)
    text = text[:start] + """            /* Mirror the original image without scalar packing/unpacking. */
            lg_mirror_right_half(unit->icon, unit->icon_w, unit->icon_h);
""" + text[end:]
    text = _replace(text, """        for ( j = 0; j < unit->icon_tiny->h; j++ ) {
            for ( i = 0; i < unit->icon_tiny->w; i++ )
                set_pixel( unit->icon_tiny,
                           i, j, 
                           get_pixel( unit->icon, scale * i, scale * j ) );
        }""", "        lg_scale_two_thirds(unit->icon, unit->icon_tiny);", "tiny sprite sampling")
    text = _replace(text, "    int i, j, icon_id;", "    int i, icon_id;", "obsolete sprite row index")
    path.write_text(text)

    # These legacy bitmap-font symbols must be single logical octets.  The RCC
    # string emitter encodes non-ASCII literals as UTF-8, so use numeric cells.
    path = source_root / "src/gui.c"
    text = path.read_text()
    no_exp = "GS_NOEXP GS_NOEXP GS_NOEXP GS_NOEXP GS_NOEXP"
    for tail in ("unit->entr", "unit->cur_ammo, unit->cur_fuel, unit->entr", "unit->cur_ammo, unit->entr"):
        pattern = no_exp + ' " ", ' + tail + ' );'
        replacement = '"%c%c%c%c%c ", ' + tail + ', 128, 128, 128, 128, 128 );'
        text = _replace(text, pattern, replacement, "private experience glyphs " + tail)
    text = _replace(text, '    static const char checkbox_indent_str[] = GS_CHECK_BOX_EMPTY " ";',
                    '    static const char checkbox_indent_str[] = {136, 32, 0};', "private checkbox glyph")
    path.write_text(text)

    path = source_root / "src/main.c"
    text = path.read_text()
    text = _replace(text, "    SDL_SetCursor( empty_cursor );", """    /* LGeneral paints its own bitmap cursors in gui_draw(). A transparent SDL
     * cursor is still visible to SDL and flushes cursor rectangles while the
     * GUI backgrounds are temporarily restored, exposing the plain underlay.
     * Disable SDL's cursor path completely; retain all game cursor artwork. */
    SDL_ShowCursor(SDL_DISABLE);""", "disable redundant SDL cursor presentation")
    path.write_text(text)
