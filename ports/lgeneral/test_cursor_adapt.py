"""Reproduce SDL's transparent-cursor flush while GUI underlays are restored."""

import argparse
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import tempfile
import unittest

from graphics_adapt import adapt


SOURCE = None
SDL_SOURCE = None


def function(text, signature):
    start = text.index(signature)
    opening = text.index("{", start)
    depth = 0
    for position in range(opening, len(text)):
        if text[position] == "{":
            depth += 1
        elif text[position] == "}":
            depth -= 1
            if depth == 0:
                return text[start:position + 1]
    raise ValueError("unterminated C function")


class CursorTests(unittest.TestCase):
    def test_visible_transparent_cursor_publishes_mid_frame_but_disabled_cursor_does_not(self):
        if SOURCE is None or SDL_SOURCE is None:
            self.skipTest("pass --source and --sdl-source for the real SDL cursor regression")
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "src").mkdir()
            for name in ("lg-sdl.c", "lg-sdl.h", "unit_lib.c", "gui.c", "main.c"):
                shutil.copyfile(SOURCE / "src" / name, root / "src" / name)
            adapt(root, c16=True)
            main = (root / "src/main.c").read_text()
            self.assertIn("SDL_ShowCursor(SDL_DISABLE);", main)
            self.assertNotIn("SDL_SetCursor( empty_cursor );", main)
            gui = (root / "src/gui.c").read_text()
            self.assertIn("image_draw( gui->cursors );", function(gui, "void gui_draw()"))
            self.assertIn("image_move( gui->cursors,", function(gui, "void gui_move_cursor("))
            sdl = (SDL_SOURCE / "src/video/SDL_cursor.c").read_text()
            cursor_functions = "\n".join(function(sdl, signature) for signature in (
                "int SDL_ShowCursor (", "void SDL_MoveCursor(",
                "void SDL_DrawCursor(SDL_Surface", "void SDL_EraseCursor(SDL_Surface",
            ))
            source = r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
typedef struct {int x,y,w,h;} SDL_Rect;
typedef struct {unsigned flags; unsigned char pixels[64*48];} SDL_Surface;
typedef struct {SDL_Rect area;int hot_x,hot_y;} SDL_Cursor;
typedef struct SDL_VideoDevice SDL_VideoDevice;
struct SDL_VideoDevice {
    void (*MoveWMCursor)(SDL_VideoDevice *,int,int);
    void (*UpdateRects)(SDL_VideoDevice *,int,SDL_Rect *);
    void (*CheckMouseMode)(SDL_VideoDevice *);
};
#define CURSOR_VISIBLE 0x01
#define CURSOR_USINGSW 0x10
#define SHOULD_DRAWCURSOR(s) (((s)&0x11)==0x11)
#define SDL_DISABLE 0
#define SDL_HWSURFACE 1
#define SDL_MUSTLOCK(s) 0
static SDL_Surface screen;
static SDL_Surface *SDL_VideoSurface=&screen;
static SDL_Cursor empty={{0,0,16,16},8,8}, *SDL_cursor=&empty;
static int SDL_cursorstate=CURSOR_VISIBLE|CURSOR_USINGSW;
static unsigned char presented[64*48];
static unsigned updates;
static void SDL_LockCursor(void) {}
static void SDL_UnlockCursor(void) {}
static int SDL_LockSurface(SDL_Surface *surface) {(void)surface;return 0;}
static void SDL_UnlockSurface(SDL_Surface *surface) {(void)surface;}
/* These are precisely the C16 SDL profile's disabled draw/erase bodies. */
static void SDL_DrawCursorNoLock(SDL_Surface *surface) {(void)surface;}
static void SDL_EraseCursorNoLock(SDL_Surface *surface) {(void)surface;}
static void SDL_MouseRect(SDL_Rect *rect) {*rect=SDL_cursor->area;}
static void update(SDL_VideoDevice *device,int count,SDL_Rect *rect) {
    int x,y;(void)device;assert(count==1);updates++;
    for(y=rect->y;y<rect->y+rect->h;y++) for(x=rect->x;x<rect->x+rect->w;x++) {
        if(x>=0 && y>=0 && x<64 && y<48)presented[y*64+x]=screen.pixels[y*64+x];
    }
}
static SDL_VideoDevice device={NULL,update,NULL}, *current_video=&device;
static void SDL_SetCursor(SDL_Cursor *cursor) {(void)cursor;}
void SDL_DrawCursor(SDL_Surface *surface);
void SDL_EraseCursor(SDL_Surface *surface);
''' + cursor_functions + r'''
int main(void) {
    unsigned i, changed=0;
    /* engine_begin_frame restores the bright map underlay. The displayed
     * framebuffer still contains the previous translucent dark dialog. */
    memset(screen.pixels,255,sizeof screen.pixels);
    memset(presented,48,sizeof presented);
    updates=0;SDL_MoveCursor(32,24);
    assert(updates==2); /* no-op cursor drawing still flushes old/new rects */
    for(i=0;i<sizeof presented;i++)if(presented[i]!=48)changed++;
    assert(changed>=16*16);
    assert(presented[(24-8)*64+(32-8)]==255);
    /* The app owns a full-colour bitmap cursor, so no SDL software cursor
     * should be considered visible during its composition loop. */
    SDL_ShowCursor(SDL_DISABLE);
    assert(!SHOULD_DRAWCURSOR(SDL_cursorstate));
    memset(presented,48,sizeof presented);updates=0;
    SDL_MoveCursor(40,30);
    assert(updates==0);
    for(i=0;i<sizeof presented;i++)assert(presented[i]==48);
    /* The app can still compose its own cursor and publish the finished GUI. */
    memset(screen.pixels,48,sizeof screen.pixels);
    screen.pixels[30*64+40]=255;
    {SDL_Rect complete={0,0,64,48};update(&device,1,&complete);}
    assert(presented[30*64+40]==255 && presented[29*64+39]==48);
    puts("transparent SDL cursor reproduced mid-frame rectangle; hidden SDL cursor preserves app bitmap");
    return 0;
}
'''
            program = root / "cursor.c"
            program.write_text(source)
            binary = root / "cursor"
            sdk_compiler = Path("/Library/Developer/CommandLineTools/usr/bin/clang")
            compiler = shlex.split(os.environ.get("CC", str(sdk_compiler) if sdk_compiler.is_file() else "cc"))
            sdk = Path("/Library/Developer/CommandLineTools/SDKs/MacOSX.sdk")
            if "CC" not in os.environ and sdk_compiler.is_file() and sdk.is_dir():
                compiler += ["-isysroot", str(sdk)]
            subprocess.run([*compiler, "-std=c99", "-Wall", "-Wextra", "-Werror",
                            "-fsanitize=address,undefined", "-fno-sanitize-recover=undefined",
                            str(program), "-o", str(binary)], check=True)
            subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--source", type=Path)
    parser.add_argument("--sdl-source", type=Path)
    args, remaining = parser.parse_known_args()
    SOURCE, SDL_SOURCE = args.source, args.sdl_source
    unittest.main(argv=[__file__, *remaining])
