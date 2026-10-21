"""Compile actual upstream weather loops and compare pixels, aliases and errors."""

import argparse
import os
from pathlib import Path
import shlex
import subprocess
import tempfile
import unittest

from terrain_adapt import adapt


SOURCE = None


class TerrainTests(unittest.TestCase):
    def test_original_pixels_shared_variants_and_partial_cleanup(self):
        if SOURCE is None:
            self.skipTest("pass --source for upstream terrain checks")
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "src").mkdir()
            original = (SOURCE / "src/terrain.c").read_text()
            (root / "src/terrain.c").write_text(original)
            adapt(root, True)
            changed = (root / "src/terrain.c").read_text()
            helper_start = changed.index("static void lg_free_weather_images")
            helper_end = changed.index("int terrain_load(", helper_start)
            helper = changed[helper_start:helper_end]
            old_start = original.index("        /* each weather type got its own image")
            old_end = original.index("        /* spot cost */", old_start)
            old_loop = original[old_start:old_end]
            new_start = changed.index("        /* Weather variants can share artwork")
            new_end = changed.index("        /* spot cost */", new_start)
            new_loop = changed[new_start:new_end]
            source = r'''
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef unsigned int Uint32;
typedef struct { int w,h; Uint32 key; unsigned char pixels[8]; } SDL_Surface;
typedef struct { SDL_Surface **images, **images_fogged; } Terrain_Type;
typedef struct { const char *id; } Weather_Type;
typedef struct { SDL_Surface *fog; } Terrain_Icons;
#define SDL_SWSURFACE 0
#define SDL_SRCCOLORKEY 1
#define FOG_ALPHA 128
#define STRCMP(a,b) (!strcmp((a),(b)))
static const char *const *current_names;
static int missing_at=-1,fail_load_at,fail_fog_at,fail_alloc_at;
static int loads,fogs,allocations,live_surfaces;
static int weather_type_count=12,hex_w=2,hex_h=2;
static Weather_Type weather_types[]={{"w0"},{"w1"},{"w2"},{"w3"},{"w4"},{"w5"},
    {"w6"},{"w7"},{"w8"},{"w9"},{"w10"},{"w11"}};
static SDL_Surface fog_pattern={2,2,0,{0,80,120,255,0,0,0,0}};
static Terrain_Icons icons={&fog_pattern}, *terrain_icons=&icons;
static SDL_Surface *draw_source,*draw_destination;
static int draw_x,draw_y,draw_w,draw_h,source_x,source_y;
static void full_destination(SDL_Surface *s) {
    draw_destination=s;draw_x=draw_y=0;draw_w=s->w;draw_h=s->h;
}
static void full_source(SDL_Surface *s) { draw_source=s;source_x=source_y=0; }
#define FULL_DEST(s) full_destination(s)
#define FULL_SOURCE(s) full_source(s)
#define DEST(s,x,y,w,h) (draw_destination=(s),draw_x=(x),draw_y=(y),draw_w=(w),draw_h=(h))
#define SOURCE(s,x,y) (draw_source=(s),source_x=(x),source_y=(y))
static int parser_get_value(void *sub,const char *path,char **str,int unused) {
    int index=atoi(path+7);(void)sub;(void)unused;
    assert(index>=0 && index<12);
    if(index==missing_at)return 0;
    *str=(char *)current_names[index];return 1;
}
static void *checked_calloc(size_t n,size_t size) {
    allocations++;if(allocations==fail_alloc_at)return NULL;return calloc(n,size);
}
#define calloc checked_calloc
static SDL_Surface *new_surface(void) {
    SDL_Surface *s=malloc(sizeof(*s));assert(s);memset(s,0,sizeof(*s));
    s->w=4;s->h=2;live_surfaces++;return s;
}
static SDL_Surface *load_surf(const char *name,int flags) {
    unsigned hash=7;int i;SDL_Surface *s;(void)flags;
    loads++;if(loads==fail_load_at)return NULL;
    s=new_surface();for(i=0;name[i];i++)hash=hash*33+(unsigned char)name[i];
    for(i=0;i<8;i++)s->pixels[i]=(unsigned char)(hash+i*17);
    return s;
}
static SDL_Surface *create_surf(int w,int h,int flags) {
    SDL_Surface *s;(void)flags;fogs++;if(fogs==fail_fog_at)return NULL;
    s=new_surface();assert(w==4 && h==2);s->w=w;s->h=h;return s;
}
static void SDL_FreeSurface(SDL_Surface *s) { assert(s);live_surfaces--;assert(live_surfaces>=0);free(s); }
static Uint32 get_pixel(SDL_Surface *s,int x,int y) {return s->pixels[y*s->w+x];}
static void SDL_SetColorKey(SDL_Surface *s,int flag,Uint32 key) {(void)flag;s->key=key;}
static void blit_surf(void) {
    int x,y;for(y=0;y<draw_h;y++)for(x=0;x<draw_w;x++)
        draw_destination->pixels[(y+draw_y)*draw_destination->w+x+draw_x]=
            draw_source->pixels[(y+source_y)*draw_source->w+x+source_x];
}
static void alpha_blit_surf(int alpha) {
    int x,y;for(y=0;y<draw_h;y++)for(x=0;x<draw_w;x++) {
        unsigned char *p=&draw_destination->pixels[(y+draw_y)*draw_destination->w+x+draw_x];
        unsigned v=draw_source->pixels[(y+source_y)*draw_source->w+x+source_x];
        *p=(unsigned char)((*p*(255-alpha)+v*alpha)/255);
    }
}
''' + helper
            for name, loop in (("prepare_original", old_loop), ("prepare_shared", new_loop)):
                source += f"static int {name}(Terrain_Type *output,const char *const *names) {{\n" + r'''
    Terrain_Type *terrain_types=output;
    int i=0,j,k,alias=0,count;
    unsigned long lg_unique_images=0;
    const char **image_names=NULL;
    void *sub=NULL;
    char path[512],*str;
    (void)alias;(void)lg_unique_images;
    current_names=names;
''' + loop + r'''
    return 1;
parser_failure:
failure:
    free(image_names);
    lg_free_weather_images(output->images,weather_type_count);output->images=NULL;
    lg_free_weather_images(output->images_fogged,weather_type_count);output->images_fogged=NULL;
    return 0;
}
'''
            source += r'''
static void cleanup(Terrain_Type *type) {
    lg_free_weather_images(type->images,weather_type_count);
    lg_free_weather_images(type->images_fogged,weather_type_count);
    type->images=type->images_fogged=NULL;
}
static void reset(void) {
    loads=fogs=allocations=0;missing_at=-1;fail_load_at=fail_fog_at=fail_alloc_at=0;
}
int main(void) {
    const char *names[]={"fair","default","fair","rain","rain","default",
                         "snow","rain","snow","fair","snow","default"};
    Terrain_Type original={0},shared={0};
    int j,reference_loads;
    reset();assert(prepare_original(&original,names));reference_loads=loads;
    reset();assert(prepare_shared(&shared,names));
    assert(loads==3 && fogs==3 && loads<reference_loads);
    assert(shared.images[0]==shared.images[1] && shared.images[0]==shared.images[2]);
    assert(shared.images[3]==shared.images[4] && shared.images[3]==shared.images[7]);
    assert(shared.images_fogged[6]==shared.images_fogged[8]);
    for(j=0;j<weather_type_count;j++) {
        assert(memcmp(original.images[j]->pixels,shared.images[j]->pixels,8)==0);
        assert(memcmp(original.images_fogged[j]->pixels,shared.images_fogged[j]->pixels,8)==0);
        assert(original.images[j]->key==shared.images[j]->key);
        assert(original.images_fogged[j]->key==shared.images_fogged[j]->key);
    }
    cleanup(&original);cleanup(&shared);assert(live_surfaces==0);
    for(j=0;j<weather_type_count;j++) {
        reset();missing_at=j;assert(!prepare_shared(&shared,names));assert(live_surfaces==0);
    }
    for(j=1;j<=3;j++) {
        reset();fail_load_at=j;assert(!prepare_shared(&shared,names));assert(live_surfaces==0);
        reset();fail_fog_at=j;assert(!prepare_shared(&shared,names));assert(live_surfaces==0);
        reset();fail_alloc_at=j;assert(!prepare_shared(&shared,names));assert(live_surfaces==0);
    }
    reset();assert(prepare_shared(&shared,names));cleanup(&shared);assert(live_surfaces==0);
    puts("weather sharing: identical pixels, 3 unique variants, all partial failures cleaned");
    return 0;
}
'''
            program = root / "terrain.c"
            program.write_text(source)
            binary = root / "terrain"
            compiler = shlex.split(os.environ.get("CC", "/Library/Developer/CommandLineTools/usr/bin/clang"
                                                  if Path("/Library/Developer/CommandLineTools/usr/bin/clang").is_file() else "cc"))
            sdk = Path("/Library/Developer/CommandLineTools/SDKs/MacOSX.sdk")
            if "CC" not in os.environ and sdk.is_dir():
                compiler += ["-isysroot", str(sdk)]
            subprocess.run([*compiler, "-std=c99", "-Wall", "-Wextra", "-Werror",
                            "-Wno-deprecated-declarations", "-Wno-unused-label",
                            "-fsanitize=address,undefined", "-fno-sanitize-recover=undefined",
                            str(program), "-o", str(binary)], check=True)
            subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--source", type=Path)
    args, remaining = parser.parse_known_args()
    SOURCE = args.source
    unittest.main(argv=[__file__, *remaining])
