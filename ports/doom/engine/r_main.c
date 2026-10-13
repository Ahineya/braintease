//
// Copyright(C) 1993-1996 Id Software, Inc.
// Copyright(C) 2005-2014 Simon Howard
//
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU General Public License
// as published by the Free Software Foundation; either version 2
// of the License, or (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// DESCRIPTION:
//	Rendering main loop and setup functions,
//	 utility functions (BSP, geometry, trigonometry).
//	See tables.c, too.
//





#include <stdlib.h>
#include <math.h>
#include <string.h>


#include "doomdef.h"
#include "d_loop.h"

#include "m_bbox.h"
#include "m_menu.h"

#include "r_local.h"
#include "r_sky.h"
#include "i_system.h"
#include "doom_build_config.h"
#include "patch_cache.h"





// Fineangles in the SCREENWIDTH wide window.
#define FIELDOFVIEW		2048	



angle_t viewangleoffset;

// increment every time a check is made
uint32_t validcount = 1;		


lighttable_t*		fixedcolormap;
extern lighttable_t**	walllights;

int			centerx;
int			centery;

fixed_t			centerxfrac;
fixed_t			centeryfrac;
fixed_t			projection;

// just for profiling purposes
int			framecount;	

int			sscount;
int			linecount;
int			loopcount;

fixed_t			viewx;
fixed_t			viewy;
fixed_t			viewz;

angle_t			viewangle;

fixed_t			viewcos;
fixed_t			viewsin;

player_t*		viewplayer;

// 0 = high, 1 = low
int			detailshift;	

//
// precalculated math tables
//
angle_t			clipangle;

// The viewangletox[viewangle + FINEANGLES/4] lookup
// maps the visible view angles to screen X coordinates,
// flattening the arc to a flat projection plane.
// There will be many angles mapped to the same X. 
int *viewangletox;

// The xtoviewangleangle[] table maps a screen pixel
// to the lowest viewangle that maps back to x ranges
// from clipangle to -clipangle.
angle_t *xtoviewangle;

lighttable_t *(*scalelight)[MAXLIGHTSCALE];
lighttable_t **scalelightfixed;
lighttable_t *(*zlight)[MAXLIGHTZ];

// bumped light from gun blasts
int			extralight;			



void (*colfunc) (void);
void (*basecolfunc) (void);
void (*fuzzcolfunc) (void);
void (*transcolfunc) (void);
void (*spanfunc) (void);



//
// R_AddPointToBox
// Expand a given bbox
// so that it encloses a given point.
//
void
R_AddPointToBox
( fixed_t x,
  fixed_t y,
  fixed_t*	box )
{
    if (x< box[BOXLEFT])
	box[BOXLEFT] = x;
    if (x> box[BOXRIGHT])
	box[BOXRIGHT] = x;
    if (y< box[BOXBOTTOM])
	box[BOXBOTTOM] = y;
    if (y> box[BOXTOP])
	box[BOXTOP] = y;
}


//
// R_PointOnSide
// Traverse BSP (sub) tree,
//  check point against partition plane.
// Returns side 0 (front) or 1 (back).
//
int
R_PointOnSide
( fixed_t	x,
  fixed_t	y,
  node_t*	node )
{
    fixed_t	dx;
    fixed_t	dy;
    fixed_t	left;
    fixed_t	right;
	
    if (!node->dx)
    {
	if (x <= node->x)
	    return node->dy > 0;
	
	return node->dy < 0;
    }
    if (!node->dy)
    {
	if (y <= node->y)
	    return node->dx < 0;
	
	return node->dx > 0;
    }
	
    dx = (x - node->x);
    dy = (y - node->y);
	
    // Try to quickly decide by looking at sign bits.
    if ( (node->dy ^ node->dx ^ dx ^ dy)&0x80000000 )
    {
	if  ( (node->dy ^ dx) & 0x80000000 )
	{
	    // (left is negative)
	    return 1;
	}
	return 0;
    }

    left = FixedMul ( node->dy>>FRACBITS , dx );
    right = FixedMul ( dy , node->dx>>FRACBITS );
	
    if (right < left)
    {
	// front side
	return 0;
    }
    // back side
    return 1;			
}


int
R_PointOnSegSide
( fixed_t	x,
  fixed_t	y,
  seg_t*	line )
{
    fixed_t	lx;
    fixed_t	ly;
    fixed_t	ldx;
    fixed_t	ldy;
    fixed_t	dx;
    fixed_t	dy;
    fixed_t	left;
    fixed_t	right;
	
    lx = line->v1->x;
    ly = line->v1->y;
	
    ldx = line->v2->x - lx;
    ldy = line->v2->y - ly;
	
    if (!ldx)
    {
	if (x <= lx)
	    return ldy > 0;
	
	return ldy < 0;
    }
    if (!ldy)
    {
	if (y <= ly)
	    return ldx < 0;
	
	return ldx > 0;
    }
	
    dx = (x - lx);
    dy = (y - ly);
	
    // Try to quickly decide by looking at sign bits.
    if ( (ldy ^ ldx ^ dx ^ dy)&0x80000000 )
    {
	if  ( (ldy ^ dx) & 0x80000000 )
	{
	    // (left is negative)
	    return 1;
	}
	return 0;
    }

    left = FixedMul ( ldy>>FRACBITS , dx );
    right = FixedMul ( dy , ldx>>FRACBITS );
	
    if (right < left)
    {
	// front side
	return 0;
    }
    // back side
    return 1;			
}


//
// R_PointToAngle
// To get a global angle from cartesian coordinates,
//  the coordinates are flipped until they are in
//  the first octant of the coordinate system, then
//  the y (<=x) is scaled and divided by x to get a
//  tangent (slope) value which is looked up in the
//  tantoangle[] table.

//




angle_t
R_PointToAngle
( fixed_t	x,
  fixed_t	y )
{	
    x -= viewx;
    y -= viewy;
    
    if ( (!x) && (!y) )
	return 0;

    if (x>= 0)
    {
	// x >=0
	if (y>= 0)
	{
	    // y>= 0

	    if (x>y)
	    {
		// octant 0
		return tantoangle[ SlopeDiv(y,x)];
	    }
	    else
	    {
		// octant 1
		return ANG90-1-tantoangle[ SlopeDiv(x,y)];
	    }
	}
	else
	{
	    // y<0
	    y = -y;

	    if (x>y)
	    {
		// octant 8
		return -tantoangle[SlopeDiv(y,x)];
	    }
	    else
	    {
		// octant 7
		return ANG270+tantoangle[ SlopeDiv(x,y)];
	    }
	}
    }
    else
    {
	// x<0
	x = -x;

	if (y>= 0)
	{
	    // y>= 0
	    if (x>y)
	    {
		// octant 3
		return ANG180-1-tantoangle[ SlopeDiv(y,x)];
	    }
	    else
	    {
		// octant 2
		return ANG90+ tantoangle[ SlopeDiv(x,y)];
	    }
	}
	else
	{
	    // y<0
	    y = -y;

	    if (x>y)
	    {
		// octant 4
		return ANG180+tantoangle[ SlopeDiv(y,x)];
	    }
	    else
	    {
		 // octant 5
		return ANG270-1-tantoangle[ SlopeDiv(x,y)];
	    }
	}
    }
    return 0;
}


angle_t
R_PointToAngle2
( fixed_t	x1,
  fixed_t	y1,
  fixed_t	x2,
  fixed_t	y2 )
{	
    viewx = x1;
    viewy = y1;
    
    return R_PointToAngle (x2, y2);
}


fixed_t
R_PointToDist
( fixed_t	x,
  fixed_t	y )
{
    int		angle;
    fixed_t	dx;
    fixed_t	dy;
    fixed_t	temp;
    fixed_t	dist;
    fixed_t     frac;
	
    dx = labs(x - viewx);
    dy = labs(y - viewy);
	
    if (dy>dx)
    {
	temp = dx;
	dx = dy;
	dy = temp;
    }

    // Fix crashes in udm1.wad

    if (dx != 0)
    {
        frac = FixedDiv(dy, dx);
    }
    else
    {
	frac = 0;
    }
	
    angle = (tantoangle[frac>>DBITS]+ANG90) >> ANGLETOFINESHIFT;

    // use as cosine
    dist = FixedDiv (dx, finesine[angle] );	
	
    return dist;
}




//
// R_InitPointToAngle
//
void R_InitPointToAngle (void)
{
    // UNUSED - now getting from tables.c
#if 0
    int	i;
    long	t;
    float	f;
//
// slope (tangent) to angle lookup
//
    for (i=0 ; i<=SLOPERANGE ; i++)
    {
	f = atan( (float)i/SLOPERANGE )/(3.141592657*2);
	t = 0xffffffff*f;
	tantoangle[i] = t;
    }
#endif
}


//
// R_ScaleFromGlobalAngle
// Returns the texture mapping scale
//  for the current line (horizontal span)
//  at the given angle.
// rw_distance must be calculated first.
//
fixed_t R_ScaleFromGlobalAngle (angle_t visangle)
{
    fixed_t		scale;
    angle_t		anglea;
    angle_t		angleb;
    fixed_t sinea;
    fixed_t sineb;
    fixed_t		num;
    fixed_t den;

    // UNUSED
#if 0
{
    fixed_t		dist;
    fixed_t		z;
    fixed_t		sinv;
    fixed_t		cosv;
	
    sinv = finesine[(visangle-rw_normalangle)>>ANGLETOFINESHIFT];	
    dist = FixedDiv (rw_distance, sinv);
    cosv = finecosine[(viewangle-visangle)>>ANGLETOFINESHIFT];
    z = abs(FixedMul (dist, cosv));
    scale = FixedDiv(projection, z);
    return scale;
}
#endif

    anglea = ANG90 + (visangle-viewangle);
    angleb = ANG90 + (visangle-rw_normalangle);

    // both sines are allways positive
    sinea = finesine[anglea>>ANGLETOFINESHIFT];	
    sineb = finesine[angleb>>ANGLETOFINESHIFT];
    num = FixedMul(projection,sineb)<<detailshift;
    den = FixedMul(rw_distance,sinea);

    if (den > num>>16)
    {
	scale = FixedDiv (num, den);

	if (scale > 64*FRACUNIT)
	    scale = 64*FRACUNIT;
	else if (scale < 256)
	    scale = 256;
    }
    else
	scale = 64*FRACUNIT;
	
    return scale;
}



//
// R_InitTables
//
void R_InitTables (void)
{
    // UNUSED: now getting from tables.c
#if 0
    int		i;
    float	a;
    float	fv;
    int		t;
    
    // viewangle tangent table
    for (i=0 ; i<FINEANGLES/2 ; i++)
    {
	a = (i-FINEANGLES/4+0.5)*PI*2/FINEANGLES;
	fv = FRACUNIT*tan (a);
	t = fv;
	finetangent[i] = t;
    }
    
    // finesine table
    for (i=0 ; i<5*FINEANGLES/4 ; i++)
    {
	// OPTIMIZE: mirror...
	a = (i+0.5)*PI*2/FINEANGLES;
	t = FRACUNIT*sin (a);
	finesine[i] = t;
    }
#endif

}



//
// R_InitTextureMapping
//
void R_InitTextureMapping (void)
{
    int			i;
    int			x;
    fixed_t t;
    fixed_t		focallength;
    
    // Use tangent table to generate viewangletox:
    //  viewangletox will give the next greatest x
    //  after the view angle.
    //
    // Calc focallength
    //  so FIELDOFVIEW angles covers SCREENWIDTH.
    focallength = FixedDiv (centerxfrac,
			    finetangent[FINEANGLES/4+FIELDOFVIEW/2] );
	
    for (i=0 ; i<FINEANGLES/2 ; i++)
    {
	if (finetangent[i] > FRACUNIT*2)
	    t = -1;
	else if (finetangent[i] < -FRACUNIT*2)
	    t = viewwidth+1;
	else
	{
	    t = FixedMul (finetangent[i], focallength);
	    t = (centerxfrac - t+FRACUNIT-1)>>FRACBITS;

	    if (t < -1)
		t = -1;
	    else if (t>viewwidth+1)
		t = viewwidth+1;
	}
	viewangletox[i] = t;
    }
    
    // Scan viewangletox[] to generate xtoviewangle[]:
    //  xtoviewangle will give the smallest view angle
    //  that maps to x.	
    // The mapping decreases monotonically. Visit x in reverse order so the
    // search cursor advances once instead of rescanning from zero per pixel.
    i = 0;
    for (x=viewwidth;x>=0;x--)
    {
	while (viewangletox[i]>x)
	    i++;
	xtoviewangle[x] = ((angle_t)i<<ANGLETOFINESHIFT)-ANG90;
    }
    
    // Take out the fencepost cases from viewangletox.
    for (i=0 ; i<FINEANGLES/2 ; i++)
    {
	if (viewangletox[i] == -1)
	    viewangletox[i] = 0;
	else if (viewangletox[i] == viewwidth+1)
	    viewangletox[i]  = viewwidth;
    }
	
    clipangle = xtoviewangle[0];
}



//
// R_InitLightTables
// Only inits the zlight table,
//  because the scalelight table changes with view size.
//
#define DISTMAP		2

#if defined(RIPPLE) && DOOM_WAD_MODE == 2
/* The build validates every serialized value against the source tables before
 * publishing the cooked image. Runtime work is contiguous reads and relocation
 * of light-level indices into the current colormap allocation. */
static uint32_t renderer_depth_offset;
static int renderer_checked;
static int renderer_view = -1;
extern int setblocks, setdetail;

static void R_CheckCookedRenderer(void)
{
    uint16_t header[16];
    if (renderer_checked) return;
    if (sizeof(int) != 1 || sizeof(fixed_t) != 2 || sizeof(angle_t) != 2
        || sizeof(void *) != 2 || SCREENWIDTH != 320 || SCREENHEIGHT != 200
        || FINEANGLES != 8192 || LIGHTLEVELS != 16 || MAXLIGHTZ != 128
        || MAXLIGHTSCALE != 48 || NUMCOLORMAPS != 32
        || DOOM_ASSET_RENDERER_WORDS < 2352UL
        || DOOM_ASSET_RENDERER_WORD > DOOM_ASSET_TOTAL_WORDS
        || DOOM_ASSET_RENDERER_WORDS > DOOM_ASSET_TOTAL_WORDS - DOOM_ASSET_RENDERER_WORD)
        I_Error("Cooked renderer tables do not match the target ABI");
    W_ReadAssetWords(DOOM_ASSET_RENDERER_WORD, header, 16UL);
    if (header[0] != 0x5256U || header[1] != 0x5442U || header[2] != 1U
        || header[3] != 18U || header[4] != 304U || header[5] != 0U
        || header[6] != 2048U)
        I_Error("Invalid cooked renderer header");
    renderer_depth_offset = DOOM_ASSET_RENDERER_WORD + 304UL;
    renderer_checked = 1;
}

static void R_ReadCookedRenderer(uint32_t offset, uint16_t *dest, uint32_t count)
{
    if (offset > DOOM_ASSET_RENDERER_WORDS || count > DOOM_ASSET_RENDERER_WORDS - offset)
        I_Error("Cooked renderer table lies outside its section");
    W_ReadAssetWords(DOOM_ASSET_RENDERER_WORD + offset, dest, count);
}

static void R_RelocateLightTable(uint32_t offset, lighttable_t **output, unsigned int count)
{
    uint16_t indices[32];
    lighttable_t *pointers[32];
    lighttable_t *colors[32];
    lighttable_t *color = colormaps;
    int n;
    for (n = 0; n < 32; ++n) {
        colors[n] = color;
        color += 256;
    }
    while (count != 0) {
        unsigned int batch = count < 32U ? count : 32U;
        W_ReadAssetWords(offset, indices, (uint32_t)batch);
        for (n = 0; n < batch; ++n) {
#if DOOM_ASSET_STRICT
            if (indices[n] >= 32U) I_Error("Invalid cooked light level");
#endif
            pointers[n] = colors[indices[n]];
        }
        memcpy(output, pointers, (size_t)batch * sizeof(*output));
        output += batch;
        offset += batch;
        count -= batch;
    }
}

static int R_LoadCookedViewTables(void)
{
    uint16_t record[16];
    uint32_t offsets[5];
    int view, n;
    if (setblocks < 3 || setblocks > 11 || setdetail < 0 || setdetail > 1)
        I_Error("Unsupported cooked renderer view size");
    view = (setblocks - 3) * 2 + setdetail;
    if (renderer_view == view) return 1;
    R_CheckCookedRenderer();
    W_ReadAssetWords(DOOM_ASSET_RENDERER_WORD + 16UL + (uint32_t)view * 16UL,
                     record, 16UL);
    if (record[0] != setblocks || record[1] != setdetail
        || record[2] != scaledviewwidth || record[3] != viewheight || record[4] != viewwidth)
        I_Error("Cooked renderer view does not match requested dimensions");
    for (n = 0; n < 5; ++n)
        offsets[n] = (uint32_t)record[6 + n * 2] + ((uint32_t)record[7 + n * 2] << 16);
    R_ReadCookedRenderer(offsets[0], (uint16_t *)viewangletox, 4096UL);
    R_ReadCookedRenderer(offsets[1], (uint16_t *)xtoviewangle, ((uint32_t)viewwidth + 1UL) * 2UL);
    R_ReadCookedRenderer(offsets[2], (uint16_t *)yslope, (uint32_t)viewheight * 2UL);
    R_ReadCookedRenderer(offsets[3], (uint16_t *)distscale, (uint32_t)viewwidth * 2UL);
    if (offsets[4] > DOOM_ASSET_RENDERER_WORDS || 768UL > DOOM_ASSET_RENDERER_WORDS - offsets[4])
        I_Error("Cooked scale lights lie outside their section");
    R_RelocateLightTable(DOOM_ASSET_RENDERER_WORD + offsets[4], &scalelight[0][0], 768U);
    clipangle = xtoviewangle[0];
    renderer_view = view;
    return 1;
}
#endif

void R_InitLightTables (void)
{
    int		i;
    int		j;
    int		level;
    int		startmap; 	
    fixed_t scale;

#if defined(RIPPLE) && DOOM_WAD_MODE == 2
    R_CheckCookedRenderer();
    R_RelocateLightTable(renderer_depth_offset, &zlight[0][0], 2048U);
    return;
#endif
    
    // Calculate the light levels to use
    //  for each level / distance combination.
    for (i=0 ; i< LIGHTLEVELS ; i++)
    {
	startmap = ((LIGHTLEVELS-1-i)*2)*NUMCOLORMAPS/LIGHTLEVELS;
	for (j=0 ; j<MAXLIGHTZ ; j++)
	{
	    scale = FixedDiv ((SCREENWIDTH/2*FRACUNIT), ((fixed_t)j+1)<<LIGHTZSHIFT);
	    scale >>= LIGHTSCALESHIFT;
	    level = startmap - scale/DISTMAP;
	    
	    if (level < 0)
		level = 0;

	    if (level >= NUMCOLORMAPS)
		level = NUMCOLORMAPS-1;

	    zlight[i][j] = colormaps + level*256;
	}
    }
}



//
// R_SetViewSize
// Do not really change anything here,
//  because it might be in the middle of a refresh.
// The change will take effect next refresh.
//
boolean		setsizeneeded;
int		setblocks;
int		setdetail;


void
R_SetViewSize
( int		blocks,
  int		detail )
{
    setsizeneeded = true;
    setblocks = blocks;
    setdetail = detail;
}


//
// R_ExecuteSetViewSize
//
void R_ExecuteSetViewSize (void)
{
    fixed_t	cosadj;
    fixed_t	dy;
    int		i;
    int		j;
    int		level;
    int		startmap; 	
    int prepared = 0;

    setsizeneeded = false;

    if (setblocks == 11)
    {
	scaledviewwidth = SCREENWIDTH;
	viewheight = SCREENHEIGHT;
    }
    else
    {
	scaledviewwidth = setblocks*32;
	viewheight = (setblocks*168/10)&~7;
    }
    
    detailshift = setdetail;
    viewwidth = scaledviewwidth>>detailshift;
	
    centery = viewheight/2;
    centerx = viewwidth/2;
    centerxfrac = ((fixed_t)centerx<<FRACBITS);
    centeryfrac = ((fixed_t)centery<<FRACBITS);
    projection = centerxfrac;

    if (!detailshift)
    {
	colfunc = basecolfunc = R_DrawColumn;
	fuzzcolfunc = R_DrawFuzzColumn;
	transcolfunc = R_DrawTranslatedColumn;
	spanfunc = R_DrawSpan;
    }
    else
    {
	colfunc = basecolfunc = R_DrawColumnLow;
	fuzzcolfunc = R_DrawFuzzColumnLow;
	transcolfunc = R_DrawTranslatedColumnLow;
	spanfunc = R_DrawSpanLow;
    }

    R_InitBuffer (scaledviewwidth, viewheight);
	
#if defined(RIPPLE) && DOOM_WAD_MODE == 2
    prepared = R_LoadCookedViewTables();
#endif
    if (!prepared) R_InitTextureMapping ();
    
    // psprite scales
    pspritescale = FRACUNIT*viewwidth/SCREENWIDTH;
    pspriteiscale = FRACUNIT*SCREENWIDTH/viewwidth;
    
    // thing clipping
    for (i=0 ; i<viewwidth ; i++)
	screenheightarray[i] = viewheight;

    if (prepared) return;
    
    // planes
    for (i=0 ; i<viewheight ; i++)
    {
	dy = (((fixed_t)i-viewheight/2)*FRACUNIT)+FRACUNIT/2;
	dy = labs(dy);
	yslope[i] = FixedDiv ( (viewwidth<<detailshift)/2*FRACUNIT, dy);
    }
	
    for (i=0 ; i<viewwidth ; i++)
    {
	cosadj = labs(finecosine[xtoviewangle[i]>>ANGLETOFINESHIFT]);
	distscale[i] = FixedDiv (FRACUNIT,cosadj);
    }
    
    // Calculate the light levels to use
    //  for each level / scale combination.
    for (i=0 ; i< LIGHTLEVELS ; i++)
    {
	startmap = ((LIGHTLEVELS-1-i)*2)*NUMCOLORMAPS/LIGHTLEVELS;
	for (j=0 ; j<MAXLIGHTSCALE ; j++)
	{
	    level = startmap - j*SCREENWIDTH/(viewwidth<<detailshift)/DISTMAP;
	    
	    if (level < 0)
		level = 0;

	    if (level >= NUMCOLORMAPS)
		level = NUMCOLORMAPS-1;

	    scalelight[i][j] = colormaps + level*256;
	}
    }
}



//
// R_Init
//



/* These buffers were static arrays upstream. Keep their zero initialization,
 * but allocate them outside the single bank addressed by GP. Sizes are C
 * storage bytes (16-bit cells on Ripple), never WAD file octet counts. */
static size_t renderer_storage_bytes;

static void *R_AllocateStorageKind(size_t count, size_t size, int zeroed)
{
    void *storage;
    size_t bytes;
#if defined(RIPPLE) && defined(DOOM_FAST_MEMORY)
    if (!Doom_AllocationSize(count, size, &bytes))
        I_Error("Renderer allocation size overflow");
#else
    if (count && size > (size_t)-1 / count)
        I_Error("Renderer allocation size overflow");
    bytes = count * size;
#endif
    if (bytes > (size_t)-1 - renderer_storage_bytes)
        I_Error("Renderer storage accounting overflow");
#if defined(RIPPLE) && defined(DOOM_FAST_MEMORY)
    storage = zeroed ? Doom_AllocZeroed(bytes) : malloc(bytes);
#else
    storage = zeroed ? calloc(count, size) : malloc(bytes);
#endif
    if (storage == NULL)
        I_Error("Unable to allocate renderer storage");
    renderer_storage_bytes += bytes;
    return storage;
}

void *R_AllocateStorage(size_t count, size_t size)
{
    return R_AllocateStorageKind(count, size, 1);
}

size_t R_StorageBytes(void)
{
    return renderer_storage_bytes;
}

/* These tables have the same permanent lifetime. A typed slab preserves each
 * member's native alignment and avoids five separate first-fit heap scans. */
typedef struct
{
    int angle_to_x[FINEANGLES / 2];
    angle_t x_to_angle[SCREENWIDTH + 1];
    lighttable_t *scale[LIGHTLEVELS][MAXLIGHTSCALE];
    lighttable_t *fixed[MAXLIGHTSCALE];
    lighttable_t *depth[LIGHTLEVELS][MAXLIGHTZ];
} renderer_view_storage_t;

static void R_InitViewStorage(void)
{
    renderer_view_storage_t *storage;
    if (viewangletox != NULL)
        return;
    storage = R_AllocateStorageKind(1, sizeof(*storage), 0);
    viewangletox = storage->angle_to_x;
    xtoviewangle = storage->x_to_angle;
    scalelight = storage->scale;
    scalelightfixed = storage->fixed;
    zlight = storage->depth;
    /* The other tables are completely written before their first read. */
    memset(scalelightfixed, 0, sizeof(storage->fixed));
}

void R_Init (void)
{
    R_InitViewStorage();
    R_InitBSPStorage();
    R_InitSpriteStorage();
    R_InitData ();
    printf (".");
    R_InitPointToAngle ();
    printf (".");
    R_InitTables ();
    // viewwidth / viewheight / detailLevel are set by the defaults
    printf (".");

    R_SetViewSize (screenblocks, detailLevel);
    R_InitPlanes ();
    printf (".");
    R_InitLightTables ();
    printf (".");
    R_InitSkyMap ();
    R_InitTranslationTables ();
    printf (".");
	
    framecount = 0;
}


//
// R_PointInSubsector
//
subsector_t*
R_PointInSubsector
( fixed_t	x,
  fixed_t	y )
{
    node_t*	node;
    int		side;
    int		nodenum;

    // single subsector is a special case
    if (!numnodes)				
	return subsectors;
		
    nodenum = numnodes-1;

    while (! (nodenum & NF_SUBSECTOR) )
    {
	node = &nodes[nodenum];
	side = R_PointOnSide (x, y, node);
	nodenum = node->children[side];
    }
	
    return &subsectors[nodenum & ~NF_SUBSECTOR];
}



//
// R_SetupFrame
//
void R_SetupFrame (player_t* player)
{		
    int		i;
    
    viewplayer = player;
    viewx = player->mo->x;
    viewy = player->mo->y;
    viewangle = player->mo->angle + viewangleoffset;
    extralight = player->extralight;

    viewz = player->viewz;
    
    viewsin = finesine[viewangle>>ANGLETOFINESHIFT];
    viewcos = finecosine[viewangle>>ANGLETOFINESHIFT];
	
    sscount = 0;
	
    if (player->fixedcolormap)
    {
	fixedcolormap =
	    colormaps
	    + player->fixedcolormap*256*sizeof(lighttable_t);
	
	walllights = scalelightfixed;

	for (i=0 ; i<MAXLIGHTSCALE ; i++)
	    scalelightfixed[i] = fixedcolormap;
    }
    else
	fixedcolormap = 0;
		
    framecount++;
    validcount++;
}



//
// R_RenderView
//
void R_RenderPlayerView (player_t* player)
{	
    R_SetupFrame (player);

    // Clear buffers.
    R_ClearClipSegs ();
    R_ClearDrawSegs ();
    R_ClearPlanes ();
    R_ClearSprites ();
    
    // check for new console commands.
    NetUpdate ();

    // The head node is the last node output.
    R_RenderBSPNode (numnodes-1);
    
    // Check for new console commands.
    NetUpdate ();
    
    R_DrawPlanes ();
    
    // Check for new console commands.
    NetUpdate ();
    
    R_DrawMasked ();

    // Check for new console commands.
    NetUpdate ();				
}
