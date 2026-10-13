/* Exact renderer snapshots for every view size/detail, repeated switches, and
 * bank-crossing destinations. Native reference builds use the frozen original
 * routines; normal builds extract the production routines from r_main.c. */
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "doom_build_config.h"
#include "r_local.h"
#include "i_system.h"
#include "wad_io.h"

#define FIELDOFVIEW 2048
int centerx, centery, detailshift, viewwidth, viewheight, scaledviewwidth;
fixed_t centerxfrac, centeryfrac, projection, pspritescale, pspriteiscale;
angle_t clipangle;
int *viewangletox;
angle_t *xtoviewangle;
fixed_t *yslope, *distscale;
short *screenheightarray;
lighttable_t *colormaps;
lighttable_t *(*scalelight)[MAXLIGHTSCALE], *(*zlight)[MAXLIGHTZ];
void (*colfunc)(void), (*basecolfunc)(void), (*fuzzcolfunc)(void);
void (*transcolfunc)(void), (*spanfunc)(void);
static WadFile wad;
static uint32_t read_words;

void I_Error(char *format, ...)
{
    va_list args;
    va_start(args, format);
    printf("RENDERER TABLES ERROR ");
    vprintf(format, args);
    va_end(args);
    putchar('\n');
    exit(1);
}
static void check(int good, char *message)
{
    if (!good) I_Error("%s", message);
}
void W_ReadAssetWords(uint32_t offset, uint16_t *dest, uint32_t count)
{
    check(wad_read_words(&wad, offset, dest, count), "read prepared words");
    read_words += count;
}
void R_DrawColumn(void) {}
void R_DrawColumnLow(void) {}
void R_DrawFuzzColumn(void) {}
void R_DrawFuzzColumnLow(void) {}
void R_DrawTranslatedColumn(void) {}
void R_DrawTranslatedColumnLow(void) {}
void R_DrawSpan(void) {}
void R_DrawSpanLow(void) {}
void R_InitBuffer(int width, int height)
{
    check(width == scaledviewwidth && height == viewheight, "view buffer size");
}

#ifdef DOOM_RENDERER_REFERENCE
#include "renderer_reference.inc"
#else
#include "renderer_tables.inc"
#endif

/* Each allocation spans three banks. On Ripple, all destinations start at the
 * last cell of the middle bank; even the first wide value crosses a boundary.
 * Native storage stays naturally aligned. Guards surround each whole array. */
static unsigned char *allocations[8];
static unsigned char *destinations[8];
static size_t lengths[8];
static int allocation_count;
static void *split_storage(size_t bytes)
{
    unsigned char *allocation;
    unsigned char *dest;
    int n = allocation_count++;
#ifdef DOOM_RENDERER_BF_SMOKE
    allocation = malloc(bytes + 2U * sizeof(uintptr_t));
#else
    allocation = malloc(192000UL);
#endif
    check(allocation != NULL && bytes < 32000UL && n < 8, "split allocation");
#ifdef DOOM_RENDERER_BF_SMOKE
    dest = allocation + sizeof(uintptr_t);
#elif defined(RIPPLE)
    dest = (unsigned char *)(((((uintptr_t)allocation >> 16) + 1UL) << 16) | 63999UL);
#else
    dest = allocation + 128000UL;
#endif
    allocations[n] = allocation;
    destinations[n] = dest;
    lengths[n] = bytes;
    dest[-1] = 93;
    dest[bytes] = 167;
    return dest;
}
static void check_guards(void)
{
    int n;
    for (n = 0; n < allocation_count; ++n)
        check(destinations[n][-1] == 93 && destinations[n][lengths[n]] == 167,
              "table destination guard");
}
#ifdef DOOM_RENDERER_BF_SMOKE
static uint32_t signature_a, signature_b;
static void sample(uint32_t value)
{
    signature_a += value;
    signature_b += signature_a;
}
static void print_smoke(void)
{
    int n;
    sample((uint32_t)viewwidth);
    sample((uint32_t)viewheight);
    sample((uint32_t)projection);
    sample((uint32_t)pspritescale);
    sample((uint32_t)pspriteiscale);
    sample((uint32_t)clipangle);
    for (n = 0; n < 4096; n += 1024) sample((uint32_t)viewangletox[n]);
    sample((uint32_t)viewangletox[4095]);
    sample(xtoviewangle[0]); sample(xtoviewangle[viewwidth / 2]); sample(xtoviewangle[viewwidth]);
    sample((uint32_t)yslope[0]); sample((uint32_t)yslope[viewheight / 2]); sample((uint32_t)yslope[viewheight - 1]);
    sample((uint32_t)distscale[0]); sample((uint32_t)distscale[viewwidth / 2]); sample((uint32_t)distscale[viewwidth - 1]);
    sample((uint32_t)(zlight[0][0] - colormaps)); sample((uint32_t)(zlight[0][1] - colormaps));
    sample((uint32_t)(zlight[8][64] - colormaps)); sample((uint32_t)(zlight[15][127] - colormaps));
    sample((uint32_t)(scalelight[0][0] - colormaps)); sample((uint32_t)(scalelight[0][47] - colormaps));
    sample((uint32_t)(scalelight[8][24] - colormaps)); sample((uint32_t)(scalelight[15][47] - colormaps));
    sample((uint32_t)screenheightarray[0]); sample((uint32_t)screenheightarray[viewwidth - 1]);
    printf("RENDERER SMOKE OK %lu %lu\n", (unsigned long)signature_a, (unsigned long)signature_b);
}
#else
static void comma(int i) { if (i) putchar(','); }
static void print_view(void)
{
    int i, j;
    printf("{\"blocks\":%d,\"detail\":%d,\"width\":%d,\"height\":%d,\"scaledwidth\":%d,"
           "\"clipangle\":%lu,\"pspritescale\":%ld,\"pspriteiscale\":%ld,\"viewangletox\":[",
           setblocks, setdetail, viewwidth, viewheight, scaledviewwidth,
           (unsigned long)clipangle, (long)pspritescale, (long)pspriteiscale);
    for (i = 0; i < FINEANGLES / 2; ++i) {
        comma(i); printf("%d", viewangletox[i]);
    }
    printf("],\"xtoviewangle\":[");
    for (i = 0; i <= viewwidth; ++i) {
        comma(i); printf("%lu", (unsigned long)xtoviewangle[i]);
    }
    printf("],\"yslope\":[");
    for (i = 0; i < viewheight; ++i) {
        comma(i); printf("%ld", (long)yslope[i]);
    }
    printf("],\"distscale\":[");
    for (i = 0; i < viewwidth; ++i) {
        comma(i); printf("%ld", (long)distscale[i]);
    }
    printf("],\"scalelight\":[");
    for (i = 0; i < LIGHTLEVELS; ++i) for (j = 0; j < MAXLIGHTSCALE; ++j) {
        comma(i * MAXLIGHTSCALE + j);
        printf("%ld", (long)(scalelight[i][j] - colormaps) / 256L);
    }
    printf("],\"screenheightarray\":[");
    for (i = 0; i < viewwidth; ++i) {
        comma(i); printf("%d", screenheightarray[i]);
    }
    printf("]}");
}
#endif

int main(void)
{
    int i;
#ifndef DOOM_RENDERER_BF_SMOKE
    int j, sample, pass;
#endif
    uint32_t previous;
    viewangletox = split_storage((FINEANGLES / 2) * sizeof(*viewangletox));
    xtoviewangle = split_storage((SCREENWIDTH + 1) * sizeof(*xtoviewangle));
    yslope = split_storage(SCREENHEIGHT * sizeof(*yslope));
    distscale = split_storage(SCREENWIDTH * sizeof(*distscale));
    screenheightarray = split_storage(SCREENWIDTH * sizeof(*screenheightarray));
    colormaps = split_storage(NUMCOLORMAPS * 256UL * sizeof(*colormaps));
    scalelight = split_storage(LIGHTLEVELS * sizeof(*scalelight));
    zlight = split_storage(LIGHTLEVELS * sizeof(*zlight));
    /* Cooked setup never uses trigonometric source tables. Omit that unrelated
     * initialization in the bounded BF fixture; fallback/native need it. */
#if !defined(DOOM_RENDERER_BF_SMOKE) || !defined(RIPPLE) || DOOM_WAD_MODE != 2
    check(Doom_InitTables(), "source lookup tables");
#endif
    check(wad_open_stream(&wad, DOOM_WAD_PATH, DOOM_WAD_LENGTH), "open asset stream");
    R_InitLightTables();
    check_guards();
#ifdef DOOM_RENDERER_BF_SMOKE
    R_SetViewSize(3, 1);
    R_ExecuteSetViewSize();
    check_guards();
    previous = read_words;
    R_SetViewSize(3, 1);
    R_ExecuteSetViewSize();
    check_guards();
    check(previous == read_words, "unchanged view reread tables");
    print_smoke();
#else
    printf("{\"zlight\":[");
    for (i = 0; i < LIGHTLEVELS; ++i) for (j = 0; j < MAXLIGHTZ; ++j) {
        comma(i * MAXLIGHTZ + j);
        printf("%ld", (long)(zlight[i][j] - colormaps) / 256L);
    }
    printf("],\"views\":[");
    for (pass = 0; pass < 2; ++pass) for (sample = 0; sample < 18; ++sample) {
        int index = pass ? 17 - sample : sample;
        R_SetViewSize(index / 2 + 3, index % 2);
        R_ExecuteSetViewSize();
        check(!setsizeneeded && projection == centerxfrac, "view state");
        check(colfunc == (setdetail ? R_DrawColumnLow : R_DrawColumn)
            && basecolfunc == colfunc
            && fuzzcolfunc == (setdetail ? R_DrawFuzzColumnLow : R_DrawFuzzColumn)
            && transcolfunc == (setdetail ? R_DrawTranslatedColumnLow : R_DrawTranslatedColumn)
            && spanfunc == (setdetail ? R_DrawSpanLow : R_DrawSpan), "drawing callbacks");
        check_guards();
        previous = read_words;
        R_SetViewSize(setblocks, setdetail);
        R_ExecuteSetViewSize();
        check_guards();
        check(previous == read_words, "unchanged view reread tables");
        comma(pass * 18 + sample);
        print_view();
    }
    printf("]}\n");
#endif
    wad_close(&wad);
#if !defined(DOOM_RENDERER_BF_SMOKE) || !defined(RIPPLE) || DOOM_WAD_MODE != 2
    Doom_FreeTables();
#endif
    for (i = 0; i < allocation_count; ++i) free(allocations[i]);
    return 0;
}
