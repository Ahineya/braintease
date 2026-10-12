/* Shared native/Ripple test of the actual R_RenderSegLoop.
 * Run from the repository root:
 *   node ports/doom/tools/test.mjs fixtures --program projection_widths
 * The Make rule extracts engine/r_segs.c with tools/fixtures.mjs and links
 * engine/m_fixed.c, using the same headers and definitions as the full port.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "projection_loop.inc"

int viewheight = 200;
int dc_x, dc_yl, dc_yh;
fixed_t dc_iscale, dc_texturemid;
byte *dc_source;
lighttable_t *dc_colormap;
static short floor_value, ceiling_value;
short *floorclip = &floor_value;
short *ceilingclip = &ceiling_value;
static visplane_t test_floor, test_ceiling;
visplane_t *floorplane = &test_floor;
visplane_t *ceilingplane = &test_ceiling;
static angle_t one_angle;
angle_t *xtoviewangle = &one_angle;
static fixed_t one_tangent;
const fixed_t *finetangent = &one_tangent;
static int columns;
static int first_y, last_y;

byte *R_GetColumn(int texture, int col)
{
    static byte pixel;
    (void) texture;
    (void) col;
    return &pixel;
}

static void check(const char *name, int good)
{
    if (!good) {
        printf("PROJECTION FAIL %s\n", name);
        exit(1);
    }
}

static void capture_column(void)
{
    check("screen-clipped draw", dc_yl >= 0 && dc_yh < 200 && dc_yl <= dc_yh);
    ++columns;
    first_y = dc_yl;
    last_y = dc_yh;
}
void (*colfunc)(void) = capture_column;

static void reset(void)
{
    rw_x = 0;
    rw_stopx = 1;
    rw_scale = FRACUNIT;
    rw_scalestep = topstep = bottomstep = pixhighstep = pixlowstep = 0;
    segtextured = maskedtexture = markfloor = markceiling = 0;
    midtexture = toptexture = bottomtexture = 0;
    topfrac = -40000L * HEIGHTUNIT;
    bottomfrac = 40000L * HEIGHTUNIT;
    floor_value = 200;
    ceiling_value = -1;
    columns = 0;
    first_y = last_y = -1;
    memset(&test_floor, 0, sizeof(test_floor));
    memset(&test_ceiling, 0, sizeof(test_ceiling));
}

int main(void)
{
    reset();
    midtexture = 1;
    R_RenderSegLoop();
    check("wide upper/lower projection", columns == 1 && first_y == 0 && last_y == 199);
    reset();
    toptexture = 1;
    pixhigh = 40000L * HEIGHTUNIT;
    R_RenderSegLoop();
    check("wide upper tier", columns == 1 && first_y == 0 && last_y == 199);
    reset();
    bottomtexture = 1;
    pixlow = -40000L * HEIGHTUNIT;
    R_RenderSegLoop();
    check("wide lower tier", columns == 1 && first_y == 0 && last_y == 199);
    reset();
    midtexture = 1;
    topfrac = 40000L * HEIGHTUNIT;
    bottomfrac = 40001L * HEIGHTUNIT;
    R_RenderSegLoop();
    check("empty below-screen wall", columns == 0);
    reset();
    midtexture = 1;
    topfrac = -40001L * HEIGHTUNIT;
    bottomfrac = -40000L * HEIGHTUNIT;
    R_RenderSegLoop();
    check("empty above-screen wall", columns == 0);
    reset();
    markceiling = 1;
    topfrac = 40000L * HEIGHTUNIT;
    R_RenderSegLoop();
    check("wide ceiling mark", test_ceiling.top[0] == 0 && test_ceiling.bottom[0] == 199);
    reset();
    markfloor = 1;
    bottomfrac = -40000L * HEIGHTUNIT;
    R_RenderSegLoop();
    check("wide floor mark", test_floor.top[0] == 0 && test_floor.bottom[0] == 199);
    puts("PROJECTION OK wide wall tiers, empty spans and plane clipping");
    return 0;
}
