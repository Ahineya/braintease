"""Exercise the adapted upstream pixel/font/sprite code with real C surfaces."""

import argparse
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import tempfile
import unittest

from graphics_adapt import adapt
from assets import decode_bmp


SOURCE = None


class GraphicsAdaptTests(unittest.TestCase):
    def test_pixel_formats_font_markers_and_sprite_mirroring(self):
        if SOURCE is None:
            self.skipTest("pass --source for upstream graphics checks")
        sdk_compiler = Path("/Library/Developer/CommandLineTools/usr/bin/clang")
        compiler = shlex.split(os.environ.get("CC", str(sdk_compiler) if sdk_compiler.is_file() else "cc"))
        sdk = Path("/Library/Developer/CommandLineTools/SDKs/MacOSX.sdk")
        if "CC" not in os.environ and sdk_compiler.is_file() and sdk.is_dir():
            compiler += ["-isysroot", str(sdk)]
        if shutil.which(compiler[0]) is None:
            self.skipTest("a host C compiler is required")
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "src").mkdir()
            for name in ("lg-sdl.c", "lg-sdl.h", "unit_lib.c", "gui.c", "main.c"):
                shutil.copyfile(SOURCE / "src" / name, root / "src" / name)
            adapt(root, c16=True)
            sdl = (root / "src/lg-sdl.c").read_text()
            units = (root / "src/unit_lib.c").read_text()
            self.assertNotIn("AT_RGB", sdl)
            self.assertNotIn("pix_buffer", units)
            header = (root / "src/lg-sdl.h").read_text()
            gui = (root / "src/gui.c").read_text()
            self.assertIn("char_offset[257]", header)
            self.assertEqual(gui.count('"%c%c%c%c%c "'), 3)
            self.assertEqual(gui.count(", 128, 128, 128, 128, 128 );"), 3)
            self.assertIn("checkbox_indent_str[] = {136, 32, 0}", gui)
            pixels = sdl[sdl.index("Uint32 set_pixel("):sdl.index("/* sdl font */")]
            marker_start = sdl.index("            SDL_GetRGB(get_pixel(new_glyphs, i,")
            marker_end = sdl.index("            pixel &=", marker_start)
            marker = sdl[marker_start:marker_end]
            helper_start = units.index("static void lg_mirror_right_half")
            helper_end = units.index("static void lg_unit_phase", helper_start)
            helpers = units[helper_start:helper_end]
            scan_start = sdl.index("        for (i = 0; i < new_glyphs->w; i++) {")
            scan_end = sdl.index("        /* now the gory part:", scan_start)
            scan = sdl[scan_start:scan_end]
            successor_start = sdl.index("        for (i = code; i <= 256; i++)")
            successor_end = sdl.index("    }", successor_start)
            successor = sdl[successor_start:successor_end]
            font_rows = []
            for path in sorted((SOURCE / "src").rglob("*font*.bmp")):
                width, height, rgb = decode_bmp(path.read_bytes())
                row = rgb[(height - 1) * width * 3:]
                font_rows.append((path.name, width, row))
            self.assertEqual(len(font_rows), 6)
            source = r'''
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef unsigned int Uint32;
typedef unsigned char Uint8;
typedef struct { int BytesPerPixel; } SDL_PixelFormat;
typedef struct { void *pixels; Uint32 pitch; int w, h; SDL_PixelFormat *format; } SDL_Surface;
typedef struct { SDL_Surface *icon; int icon_w, icon_h; } Unit;
typedef struct { int char_offset[257]; int width; char keys[256]; } Font;
#define tr(s) (s)
static void SDL_GetRGB(Uint32 p, SDL_PixelFormat *f, Uint8 *r, Uint8 *g, Uint8 *b) {
    (void)f; *r=(Uint8)(p>>16); *g=(Uint8)(p>>8); *b=(Uint8)p;
}
''' + pixels + helpers + r'''
static Uint32 font_marker(SDL_Surface *new_glyphs, int i) {
    Uint32 pixel = 0;
    Uint8 red, green, blue;
''' + marker + r'''
    return pixel;
}
static void scan_font(const unsigned char *row, int width) {
    SDL_PixelFormat format = {3};
    SDL_Surface glyphs = {0, (Uint32)width*3, width, 1, &format};
    SDL_Surface *new_glyphs = &glyphs;
    Font storage, *font=&storage;
    unsigned code = row[0];
    unsigned char start_code = row[0];
    int i, pre_width=0, post_width=0, fixup;
    Uint32 transparency_key = 0;
    const char *path="upstream font";
    memset(font,0,sizeof(*font));
    glyphs.pixels=malloc(glyphs.pitch);
    for(i=0;i<width;i++) {
        set_pixel(&glyphs,i,0,((Uint32)row[i*3]<<16)|((Uint32)row[i*3+1]<<8)|row[i*3+2]);
    }
''' + scan + successor + r'''
    assert(code==256);
    assert(font->char_offset[256]==width);
    assert(font->width==width);
    assert(font->char_offset[255] < width);
    assert(post_width==0);
    free(glyphs.pixels);
}
'''
            for index, (name, width, row) in enumerate(font_rows):
                source += f"/* {name} */\nstatic const unsigned char font_row_{index}[] = {{" + ",".join(map(str, row)) + "};\n"
            source += r'''
int main(void) {
    int bytes;
    for (bytes=2; bytes<=4; bytes++) {
        SDL_PixelFormat format = {bytes};
        SDL_Surface surface = {0, (Uint32)(6*bytes+3), 6, 20000, &format};
        Uint32 length = surface.pitch * (Uint32)surface.h;
        Uint32 expected = bytes == 2 ? 0xb2c3 : 0xa1b2c3;
        Unit unit = {&surface, 3, surface.h};
        surface.pixels = malloc(length);
        assert(surface.pixels);
        memset(surface.pixels, 0x55, length);
        set_pixel(&surface, 2, 19999, 0xa1b2c3);
        assert(get_pixel(&surface, 2, 19999) == expected);
        assert(((Uint8 *)surface.pixels)[surface.pitch*19999U+2U*bytes] == 0xc3);
        assert(get_pixel(&surface, -1, 0) == 0);
        set_pixel(&surface, 6, 0, 0);
        assert(((Uint8 *)surface.pixels)[6*bytes] == 0x55);
        set_pixel(&surface, 0, 0, 0x1111);
        set_pixel(&surface, 1, 0, 0x2222);
        set_pixel(&surface, 2, 0, 0x3333);
        lg_mirror_right_half(unit.icon, unit.icon_w, unit.icon_h);
        assert(get_pixel(&surface, 3, 0) == 0x3333);
        assert(get_pixel(&surface, 4, 0) == 0x2222);
        assert(get_pixel(&surface, 5, 0) == 0x1111);
        assert(get_pixel(&surface, 3, 19999) == expected);
        {
            SDL_Surface scaled = {0, (Uint32)(4*bytes+3), 4, 13333, &format};
            SDL_Surface reference = scaled;
            Uint32 scaled_length = scaled.pitch * (Uint32)scaled.h;
            int x, y;
            scaled.pixels=malloc(scaled_length);
            reference.pixels=malloc(scaled_length);
            assert(scaled.pixels && reference.pixels);
            memset(scaled.pixels, 0x37, scaled_length);
            memset(reference.pixels, 0x37, scaled_length);
            lg_scale_two_thirds(&surface, &scaled);
            for(y=0;y<reference.h;y++) {
                for(x=0;x<reference.w;x++)
                    set_pixel(&reference,x,y,get_pixel(&surface,(int)(1.5*x),(int)(1.5*y)));
            }
            assert(memcmp(scaled.pixels,reference.pixels,scaled_length)==0);
            free(scaled.pixels); free(reference.pixels);
        }
        if (bytes >= 3) {
            set_pixel(&surface, 0, surface.h-1, 0xff00ff);
            set_pixel(&surface, 1, surface.h-1, 0xff0000);
            assert(font_marker(&surface, 0) == 0xff00ff);
            assert(font_marker(&surface, 1) == 0xff0000);
        }
        free(surface.pixels);
    }
'''
            for index, (_, width, _) in enumerate(font_rows):
                source += f"    scan_font(font_row_{index}, {width});\n"
            source += r'''
    return 0;
}
'''
            program = root / "pixels.c"
            program.write_text(source)
            binary = root / "pixels"
            subprocess.run([*compiler, "-std=c99", "-Wall", "-Wextra", "-Werror",
                            "-fsanitize=undefined", "-fno-sanitize-recover=undefined",
                            str(program), "-o", str(binary)], check=True)
            subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--source", type=Path)
    args, remaining = parser.parse_known_args()
    SOURCE = args.source
    unittest.main(argv=[__file__, *remaining])
