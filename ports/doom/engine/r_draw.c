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
//	The actual span/column drawing functions.
//	Here find the main potential for optimization,
//	 e.g. inline assembly, different algorithms.
//




#include <stdlib.h>

#include "doomdef.h"
#include "deh_main.h"

#include "i_system.h"
#include "z_zone.h"
#include "w_wad.h"
#include "patch_cache.h"
#include "doom_resolution.h"
#ifdef RIPPLE
#include "render_fast.h"
#endif

#include "r_local.h"

// Needs access to LFB (guess what).
#include "v_video.h"

// State.
#include "doomstat.h"


// ?
#define MAXWIDTH			1120
#define MAXHEIGHT			832

// status bar height at bottom of screen
#define SBARHEIGHT		32

//
// All drawing to the view buffer is accomplished in this file.
// The other refresh files only know about ccordinates,
//  not the architecture of the frame buffer.
// Conveniently, the frame buffer is a linear one,
//  and we need only the base address,
//  and the total size == width*height*depth/8.,
//


byte*		viewimage; 
int		viewwidth;
int		scaledviewwidth;
int		viewheight;
int		viewwindowx;
int		viewwindowy; 
byte**		ylookup; 
int*		columnofs; 

// Color tables for different players,
//  translate a limited part to another
//  (color ramps used for  suit colors).
//
byte		translations[3][256];	
 
// Backing buffer containing the bezel drawn around the screen and 
// surrounding background.

static byte *background_buffer = NULL;


//
// R_DrawColumn
// Source is the top of the column to scale.
//
lighttable_t*		dc_colormap; 
int			dc_x; 
int			dc_yl; 
int			dc_yh; 
fixed_t			dc_iscale; 
fixed_t			dc_texturemid;

// first pixel in a column (possibly virtual) 
byte*			dc_source;		

// just for profiling 
int			dccount;


#if DOOM_RESOLUTION_SCALE > 1
/* Keep the 320x200 projection and cooked lookup tables. Only absolute screen
 * coordinates on the output lattice contain pixels; all other cells are
 * deliberately left untouched. Low detail already combines two columns. */
static int R_SampleColumnStart(int low)
{
    int x;
    int first;
    if (dc_yh < dc_yl) return dc_yl;
#ifdef RANGECHECK
    if (dc_x < 0 || dc_x >= (SCREENWIDTH >> low)
     || dc_yl < 0 || dc_yh >= SCREENHEIGHT)
        I_Error("R_DrawSampledColumn: %i to %i at %i", dc_yl, dc_yh, dc_x);
#endif
    x = viewwindowx + (dc_x << low);
    if (x & (DOOM_RESOLUTION_SCALE - 1)) return dc_yh + 1;
    first = dc_yl + ((-(dc_yl + viewwindowy)) & (DOOM_RESOLUTION_SCALE - 1));
    return first;
}

static void R_DrawSampledColumn(int low)
{
    int first = R_SampleColumnStart(low);
    unsigned int count;
    uint32_t frac, step;
    byte *dest;
    if (first > dc_yh) return;
    count = (dc_yh - first) / DOOM_RESOLUTION_SCALE + 1;
    dest = ylookup[first] + columnofs[dc_x << low];
    frac = (uint32_t)dc_texturemid + (uint32_t)(first - centery) * (uint32_t)dc_iscale;
    step = (uint32_t)dc_iscale * DOOM_RESOLUTION_SCALE;
#ifdef RIPPLE
    Doom_DrawColumnStrided(dest, dc_source, dc_colormap, frac, step, count,
                           SCREENWIDTH * DOOM_RESOLUTION_SCALE);
#else
    while (count--)
    {
        *dest = dc_colormap[dc_source[(frac >> FRACBITS) & 127]];
        dest += SCREENWIDTH * DOOM_RESOLUTION_SCALE;
        frac += step;
    }
#endif
}
#endif

//
// A column is a vertical slice/span from a wall texture that,
//  given the DOOM style restrictions on the view orientation,
//  will always have constant z depth.
// Thus a special case loop for very fast rendering can
//  be used. It has also been used with Wolfenstein 3D.
// 
void R_DrawColumn (void) 
{
#if DOOM_RESOLUTION_SCALE > 1
    R_DrawSampledColumn(0);
#else 
    int			count; 
    byte*		dest; 
    fixed_t		frac;
    fixed_t		fracstep;	 
 
    count = dc_yh - dc_yl; 

    // Zero length, column does not exceed a pixel.
    if (count < 0) 
	return; 
				 
#ifdef RANGECHECK 
    if ((unsigned)dc_x >= SCREENWIDTH
	|| dc_yl < 0
	|| dc_yh >= SCREENHEIGHT) 
	I_Error ("R_DrawColumn: %i to %i at %i", dc_yl, dc_yh, dc_x); 
#endif 

    // Framebuffer destination address.
    // Use ylookup LUT to avoid multiply with ScreenWidth.
    // Use columnofs LUT for subwindows? 
    dest = ylookup[dc_yl] + columnofs[dc_x];  

    // Determine scaling,
    //  which is the only mapping to be done.
    fracstep = dc_iscale; 
    frac = dc_texturemid + (dc_yl-centery)*fracstep;
#ifdef RIPPLE
    Doom_DrawColumn(dest, dc_source, dc_colormap, (uint32_t)frac,
                    (uint32_t)fracstep, (unsigned int)(count + 1));
    return;
#endif

    // Inner loop that does the actual texture mapping,
    //  e.g. a DDA-lile scaling.
    // This is as fast as it gets.
    do 
    {
	// Re-map color indices from wall texture column
	//  using a lighting/special effects LUT.
	*dest = dc_colormap[dc_source[(frac>>FRACBITS)&127]];
	
	dest += SCREENWIDTH; 
	frac += fracstep;
	
    } while (count--); 
#endif
} 



// UNUSED.
// Loop unrolled.
#if 0
void R_DrawColumn (void) 
{ 
    int			count; 
    byte*		source;
    byte*		dest;
    byte*		colormap;
    
    unsigned		frac;
    unsigned		fracstep;
    unsigned		fracstep2;
    unsigned		fracstep3;
    unsigned		fracstep4;	 
 
    count = dc_yh - dc_yl + 1; 

    source = dc_source;
    colormap = dc_colormap;		 
    dest = ylookup[dc_yl] + columnofs[dc_x];  
	 
    fracstep = dc_iscale<<9; 
    frac = (dc_texturemid + (dc_yl-centery)*dc_iscale)<<9; 
 
    fracstep2 = fracstep+fracstep;
    fracstep3 = fracstep2+fracstep;
    fracstep4 = fracstep3+fracstep;
	
    while (count >= 8) 
    { 
	dest[0] = colormap[source[frac>>25]]; 
	dest[SCREENWIDTH] = colormap[source[(frac+fracstep)>>25]]; 
	dest[SCREENWIDTH*2] = colormap[source[(frac+fracstep2)>>25]]; 
	dest[SCREENWIDTH*3] = colormap[source[(frac+fracstep3)>>25]];
	
	frac += fracstep4; 

	dest[SCREENWIDTH*4] = colormap[source[frac>>25]]; 
	dest[SCREENWIDTH*5] = colormap[source[(frac+fracstep)>>25]]; 
	dest[SCREENWIDTH*6] = colormap[source[(frac+fracstep2)>>25]]; 
	dest[SCREENWIDTH*7] = colormap[source[(frac+fracstep3)>>25]]; 

	frac += fracstep4; 
	dest += SCREENWIDTH*8; 
	count -= 8;
    } 
	
    while (count > 0)
    { 
	*dest = colormap[source[frac>>25]]; 
	dest += SCREENWIDTH; 
	frac += fracstep; 
	count--;
    } 
}
#endif


void R_DrawColumnLow (void) 
{
#if DOOM_RESOLUTION_SCALE > 1
    R_DrawSampledColumn(1);
#else 
    int			count; 
    byte*		dest; 
    byte*		dest2;
    fixed_t		frac;
    fixed_t		fracstep;	 
    int                 x;
 
    count = dc_yh - dc_yl; 

    // Zero length.
    if (count < 0) 
	return; 
				 
#ifdef RANGECHECK 
    if ((unsigned)dc_x >= SCREENWIDTH
	|| dc_yl < 0
	|| dc_yh >= SCREENHEIGHT)
    {
	
	I_Error ("R_DrawColumn: %i to %i at %i", dc_yl, dc_yh, dc_x);
    }
    //	dccount++; 
#endif 
    // Blocky mode, need to multiply by 2.
    x = dc_x << 1;
    
    dest = ylookup[dc_yl] + columnofs[x];
#ifndef RIPPLE
    dest2 = ylookup[dc_yl] + columnofs[x+1];
#endif
    
    fracstep = dc_iscale; 
    frac = dc_texturemid + (dc_yl-centery)*fracstep;
#ifdef RIPPLE
    Doom_DrawColumnLow(dest, dc_source, dc_colormap, (uint32_t)frac,
                       (uint32_t)fracstep, (unsigned int)(count + 1));
    return;
#endif
    
    do 
    {
	// Hack. Does not work corretly.
	*dest2 = *dest = dc_colormap[dc_source[(frac>>FRACBITS)&127]];
	dest += SCREENWIDTH;
	dest2 += SCREENWIDTH;
	frac += fracstep; 

    } while (count--);
#endif
}


//
// Spectre/Invisibility.
//
#define FUZZTABLE		50 
#define FUZZOFF	(SCREENWIDTH)


int	fuzzoffset[FUZZTABLE] =
{
    FUZZOFF,-FUZZOFF,FUZZOFF,-FUZZOFF,FUZZOFF,FUZZOFF,-FUZZOFF,
    FUZZOFF,FUZZOFF,-FUZZOFF,FUZZOFF,FUZZOFF,FUZZOFF,-FUZZOFF,
    FUZZOFF,FUZZOFF,FUZZOFF,-FUZZOFF,-FUZZOFF,-FUZZOFF,-FUZZOFF,
    FUZZOFF,-FUZZOFF,-FUZZOFF,FUZZOFF,FUZZOFF,FUZZOFF,FUZZOFF,-FUZZOFF,
    FUZZOFF,-FUZZOFF,FUZZOFF,FUZZOFF,-FUZZOFF,-FUZZOFF,FUZZOFF,
    FUZZOFF,-FUZZOFF,-FUZZOFF,-FUZZOFF,-FUZZOFF,FUZZOFF,FUZZOFF,
    FUZZOFF,FUZZOFF,-FUZZOFF,FUZZOFF,FUZZOFF,-FUZZOFF,FUZZOFF 
}; 

int	fuzzpos = 0; 

#if DOOM_RESOLUTION_SCALE > 1
static void R_DrawSampledFuzzColumn(int low)
{
    int first, last, count;
    byte *dest;
    /* Fuzz depends on a neighboring pixel. Read a neighboring output row,
     * never an uncomputed row. Its phase advances once per output sample,
     * intentionally defining a coarser effect rather than full-size fuzz. */
    first = R_SampleColumnStart(low);
    if (first < DOOM_RESOLUTION_SCALE) first += DOOM_RESOLUTION_SCALE;
    last = dc_yh < viewheight - DOOM_RESOLUTION_SCALE - 1
         ? dc_yh : viewheight - DOOM_RESOLUTION_SCALE - 1;
    if (first > last) return;
    count = (last - first) / DOOM_RESOLUTION_SCALE + 1;
    dest = ylookup[first] + columnofs[dc_x << low];
    while (count--)
    {
        *dest = colormaps[6 * 256 + dest[fuzzoffset[fuzzpos] * DOOM_RESOLUTION_SCALE]];
        if (++fuzzpos == FUZZTABLE) fuzzpos = 0;
        dest += SCREENWIDTH * DOOM_RESOLUTION_SCALE;
    }
}
#endif




//
// Framebuffer postprocessing.
// Creates a fuzzy image by copying pixels
//  from adjacent ones to left and right.
// Used with an all black colormap, this
//  could create the SHADOW effect,
//  i.e. spectres and invisible players.
//
void R_DrawFuzzColumn (void) 
{
#if DOOM_RESOLUTION_SCALE > 1
    R_DrawSampledFuzzColumn(0);
#else 
    int			count; 
    byte*		dest; 
    fixed_t		frac;
    fixed_t		fracstep;	 

    // Adjust borders. Low... 
    if (!dc_yl) 
	dc_yl = 1;

    // .. and high.
    if (dc_yh == viewheight-1) 
	dc_yh = viewheight - 2; 
		 
    count = dc_yh - dc_yl; 

    // Zero length.
    if (count < 0) 
	return; 

#ifdef RANGECHECK 
    if ((unsigned)dc_x >= SCREENWIDTH
	|| dc_yl < 0 || dc_yh >= SCREENHEIGHT)
    {
	I_Error ("R_DrawFuzzColumn: %i to %i at %i",
		 dc_yl, dc_yh, dc_x);
    }
#endif
    
    dest = ylookup[dc_yl] + columnofs[dc_x];

    // Looks familiar.
    fracstep = dc_iscale; 
    frac = dc_texturemid + (dc_yl-centery)*fracstep; 

    // Looks like an attempt at dithering,
    //  using the colormap #6 (of 0-31, a bit
    //  brighter than average).
    do 
    {
	// Lookup framebuffer, and retrieve
	//  a pixel that is either one column
	//  left or right of the current one.
	// Add index from colormap to index.
	*dest = colormaps[6*256+dest[fuzzoffset[fuzzpos]]]; 

	// Clamp table lookup index.
	if (++fuzzpos == FUZZTABLE) 
	    fuzzpos = 0;
	
	dest += SCREENWIDTH;

	frac += fracstep; 
    } while (count--); 
#endif
} 

// low detail mode version
 
void R_DrawFuzzColumnLow (void) 
{
#if DOOM_RESOLUTION_SCALE > 1
    R_DrawSampledFuzzColumn(1);
#else 
    int			count; 
    byte*		dest; 
    byte*		dest2; 
    fixed_t		frac;
    fixed_t		fracstep;	 
    int x;

    // Adjust borders. Low... 
    if (!dc_yl) 
	dc_yl = 1;

    // .. and high.
    if (dc_yh == viewheight-1) 
	dc_yh = viewheight - 2; 
		 
    count = dc_yh - dc_yl; 

    // Zero length.
    if (count < 0) 
	return; 

    // low detail mode, need to multiply by 2
    
    x = dc_x << 1;
    
#ifdef RANGECHECK 
    if ((unsigned)x >= SCREENWIDTH
	|| dc_yl < 0 || dc_yh >= SCREENHEIGHT)
    {
	I_Error ("R_DrawFuzzColumn: %i to %i at %i",
		 dc_yl, dc_yh, dc_x);
    }
#endif
    
    dest = ylookup[dc_yl] + columnofs[x];
    dest2 = ylookup[dc_yl] + columnofs[x+1];

    // Looks familiar.
    fracstep = dc_iscale; 
    frac = dc_texturemid + (dc_yl-centery)*fracstep; 

    // Looks like an attempt at dithering,
    //  using the colormap #6 (of 0-31, a bit
    //  brighter than average).
    do 
    {
	// Lookup framebuffer, and retrieve
	//  a pixel that is either one column
	//  left or right of the current one.
	// Add index from colormap to index.
	*dest = colormaps[6*256+dest[fuzzoffset[fuzzpos]]]; 
	*dest2 = colormaps[6*256+dest2[fuzzoffset[fuzzpos]]]; 

	// Clamp table lookup index.
	if (++fuzzpos == FUZZTABLE) 
	    fuzzpos = 0;
	
	dest += SCREENWIDTH;
	dest2 += SCREENWIDTH;

	frac += fracstep; 
    } while (count--); 
#endif
} 
 
  
  
 

//
// R_DrawTranslatedColumn
// Used to draw player sprites
//  with the green colorramp mapped to others.
// Could be used with different translation
//  tables, e.g. the lighter colored version
//  of the BaronOfHell, the HellKnight, uses
//  identical sprites, kinda brightened up.
//
byte*	dc_translation;
byte*	translationtables;

#if DOOM_RESOLUTION_SCALE > 1
static void R_DrawSampledTranslatedColumn(int low)
{
    int first = R_SampleColumnStart(low);
    unsigned int count;
    uint32_t frac, step;
    byte *dest;
    if (first > dc_yh) return;
    count = (dc_yh - first) / DOOM_RESOLUTION_SCALE + 1;
    dest = ylookup[first] + columnofs[dc_x << low];
    frac = (uint32_t)dc_texturemid + (uint32_t)(first - centery) * (uint32_t)dc_iscale;
    step = (uint32_t)dc_iscale * DOOM_RESOLUTION_SCALE;
    while (count--)
    {
        *dest = dc_colormap[dc_translation[dc_source[frac >> FRACBITS]]];
        dest += SCREENWIDTH * DOOM_RESOLUTION_SCALE;
        frac += step;
    }
}
#endif



void R_DrawTranslatedColumn (void) 
{
#if DOOM_RESOLUTION_SCALE > 1
    R_DrawSampledTranslatedColumn(0);
#else 
    int			count; 
    byte*		dest; 
    fixed_t		frac;
    fixed_t		fracstep;	 
 
    count = dc_yh - dc_yl; 
    if (count < 0) 
	return; 
				 
#ifdef RANGECHECK 
    if ((unsigned)dc_x >= SCREENWIDTH
	|| dc_yl < 0
	|| dc_yh >= SCREENHEIGHT)
    {
	I_Error ( "R_DrawColumn: %i to %i at %i",
		  dc_yl, dc_yh, dc_x);
    }
    
#endif 


    dest = ylookup[dc_yl] + columnofs[dc_x]; 

    // Looks familiar.
    fracstep = dc_iscale; 
    frac = dc_texturemid + (dc_yl-centery)*fracstep; 

    // Here we do an additional index re-mapping.
    do 
    {
	// Translation tables are used
	//  to map certain colorramps to other ones,
	//  used with PLAY sprites.
	// Thus the "green" ramp of the player 0 sprite
	//  is mapped to gray, red, black/indigo. 
	*dest = dc_colormap[dc_translation[dc_source[frac>>FRACBITS]]];
	dest += SCREENWIDTH;
	
	frac += fracstep; 
    } while (count--); 
#endif
} 

void R_DrawTranslatedColumnLow (void) 
{
#if DOOM_RESOLUTION_SCALE > 1
    R_DrawSampledTranslatedColumn(1);
#else 
    int			count; 
    byte*		dest; 
    byte*		dest2; 
    fixed_t		frac;
    fixed_t		fracstep;	 
    int                 x;
 
    count = dc_yh - dc_yl; 
    if (count < 0) 
	return; 

    // low detail, need to scale by 2
    x = dc_x << 1;
				 
#ifdef RANGECHECK 
    if ((unsigned)x >= SCREENWIDTH
	|| dc_yl < 0
	|| dc_yh >= SCREENHEIGHT)
    {
	I_Error ( "R_DrawColumn: %i to %i at %i",
		  dc_yl, dc_yh, x);
    }
    
#endif 


    dest = ylookup[dc_yl] + columnofs[x]; 
    dest2 = ylookup[dc_yl] + columnofs[x+1]; 

    // Looks familiar.
    fracstep = dc_iscale; 
    frac = dc_texturemid + (dc_yl-centery)*fracstep; 

    // Here we do an additional index re-mapping.
    do 
    {
	// Translation tables are used
	//  to map certain colorramps to other ones,
	//  used with PLAY sprites.
	// Thus the "green" ramp of the player 0 sprite
	//  is mapped to gray, red, black/indigo. 
	*dest = dc_colormap[dc_translation[dc_source[frac>>FRACBITS]]];
	*dest2 = dc_colormap[dc_translation[dc_source[frac>>FRACBITS]]];
	dest += SCREENWIDTH;
	dest2 += SCREENWIDTH;
	
	frac += fracstep; 
    } while (count--); 
#endif
} 




//
// R_InitTranslationTables
// Creates the translation tables to map
//  the green color ramp to gray, brown, red.
// Assumes a given structure of the PLAYPAL.
// Could be read from a lump instead.
//
void R_InitTranslationTables (void)
{
    int		i;
	
    translationtables = Z_Malloc (256*3, PU_STATIC, 0);
    
    // translate just the 16 green colors
    for (i=0 ; i<256 ; i++)
    {
	if (i >= 0x70 && i<= 0x7f)
	{
	    // map green ramp to gray, brown, red
	    translationtables[i] = 0x60 + (i&0xf);
	    translationtables [i+256] = 0x40 + (i&0xf);
	    translationtables [i+512] = 0x20 + (i&0xf);
	}
	else
	{
	    // Keep all other colors as is.
	    translationtables[i] = translationtables[i+256] 
		= translationtables[i+512] = i;
	}
    }
}




//
// R_DrawSpan 
// With DOOM style restrictions on view orientation,
//  the floors and ceilings consist of horizontal slices
//  or spans with constant z depth.
// However, rotation around the world z axis is possible,
//  thus this mapping, while simpler and faster than
//  perspective correct texture mapping, has to traverse
//  the texture at an angle in all but a few cases.
// In consequence, flats are not stored by column (like walls),
//  and the inner loop has to step in texture space u and v.
//
int			ds_y; 
int			ds_x1; 
int			ds_x2;

lighttable_t*		ds_colormap; 

fixed_t			ds_xfrac; 
fixed_t			ds_yfrac; 
fixed_t			ds_xstep; 
fixed_t			ds_ystep;

// start of a 64*64 tile image 
byte*			ds_source;	

// just for profiling
int			dscount;



#if DOOM_RESOLUTION_SCALE > 1
static void R_DrawSampledSpan(int low)
{
    uint32_t position, step;
    byte *dest;
    unsigned int count, skip, increment;
    int first;
#ifdef RANGECHECK
    if (ds_x2 < ds_x1 || ds_x1 < 0 || ds_x2 >= (SCREENWIDTH >> low)
     || ds_y < 0 || ds_y >= SCREENHEIGHT)
        I_Error("R_DrawSampledSpan: %i to %i at %i", ds_x1, ds_x2, ds_y);
#endif
    if ((ds_y + viewwindowy) & (DOOM_RESOLUTION_SCALE - 1)) return;
    skip = ((-(viewwindowx + (ds_x1 << low))) & (DOOM_RESOLUTION_SCALE - 1)) >> low;
    first = ds_x1 + skip;
    if (first > ds_x2) return;
    increment = DOOM_RESOLUTION_SCALE >> low;
    count = (ds_x2 - first) / increment + 1;
    /* Offset the already packed position, preserving upstream carry and
     * rounding. Repacking multiplied x/y steps changes selected texels. */
    position = (((uint32_t)ds_xfrac << 10) & 0xffff0000UL)
             | (((uint32_t)ds_yfrac >> 6) & 0xffffUL);
    step = (((uint32_t)ds_xstep << 10) & 0xffff0000UL)
         | (((uint32_t)ds_ystep >> 6) & 0xffffUL);
    position += step * skip;
    step *= increment;
    dest = ylookup[ds_y] + columnofs[first << low];
#ifdef RIPPLE
    Doom_DrawSpanStrided(dest, ds_source, ds_colormap, position, step, count,
                         DOOM_RESOLUTION_SCALE);
#else
    while (count--)
    {
        unsigned int spot = ((position >> 4) & 4032UL) | (position >> 26);
        *dest = ds_colormap[ds_source[spot]];
        dest += DOOM_RESOLUTION_SCALE;
        position += step;
    }
#endif
}
#endif

//
// Draws the actual span.
void R_DrawSpan (void) 
{
#if DOOM_RESOLUTION_SCALE > 1
    R_DrawSampledSpan(0);
#else 
    uint32_t position, step;
    byte *dest;
    int count;
    int spot;
    uint32_t xtemp, ytemp;

#ifdef RANGECHECK
    if (ds_x2 < ds_x1
	|| ds_x1<0
	|| ds_x2>=SCREENWIDTH
	|| (unsigned)ds_y>SCREENHEIGHT)
    {
	I_Error( "R_DrawSpan: %i to %i at %i",
		 ds_x1,ds_x2,ds_y);
    }
//	dscount++;
#endif

    // Pack position and step variables into a single 32-bit integer,
    // with x in the top 16 bits and y in the bottom 16 bits.  For
    // each 16-bit part, the top 6 bits are the integer part and the
    // bottom 10 bits are the fractional part of the pixel position.

    position = (((uint32_t) ds_xfrac << 10) & 0xffff0000)
             | ((ds_yfrac >> 6)  & 0x0000ffff);
    step = (((uint32_t) ds_xstep << 10) & 0xffff0000)
         | ((ds_ystep >> 6)  & 0x0000ffff);

    dest = ylookup[ds_y] + columnofs[ds_x1];

    // We do not check for zero spans here?
    count = ds_x2 - ds_x1;
#ifdef RIPPLE
    Doom_DrawSpan(dest, ds_source, ds_colormap, position, step,
                  (unsigned int)(count + 1));
    return;
#endif

    do
    {
	// Calculate current texture index in u,v.
        ytemp = (position >> 4) & 0x0fc0;
        xtemp = (position >> 26);
        spot = xtemp | ytemp;

	// Lookup pixel from flat texture tile,
	//  re-index using light/colormap.
	*dest++ = ds_colormap[ds_source[spot]];

        position += step;

    } while (count--);
#endif
}



// UNUSED.
// Loop unrolled by 4.
#if 0
void R_DrawSpan (void) 
{ 
    unsigned	position, step;

    byte*	source;
    byte*	colormap;
    byte*	dest;
    
    unsigned	count;
    usingned	spot; 
    unsigned	value;
    unsigned	temp;
    unsigned	xtemp;
    unsigned	ytemp;
		
    position = ((ds_xfrac<<10)&0xffff0000) | ((ds_yfrac>>6)&0xffff);
    step = ((ds_xstep<<10)&0xffff0000) | ((ds_ystep>>6)&0xffff);
		
    source = ds_source;
    colormap = ds_colormap;
    dest = ylookup[ds_y] + columnofs[ds_x1];	 
    count = ds_x2 - ds_x1 + 1; 
	
    while (count >= 4) 
    { 
	ytemp = position>>4;
	ytemp = ytemp & 4032;
	xtemp = position>>26;
	spot = xtemp | ytemp;
	position += step;
	dest[0] = colormap[source[spot]]; 

	ytemp = position>>4;
	ytemp = ytemp & 4032;
	xtemp = position>>26;
	spot = xtemp | ytemp;
	position += step;
	dest[1] = colormap[source[spot]];
	
	ytemp = position>>4;
	ytemp = ytemp & 4032;
	xtemp = position>>26;
	spot = xtemp | ytemp;
	position += step;
	dest[2] = colormap[source[spot]];
	
	ytemp = position>>4;
	ytemp = ytemp & 4032;
	xtemp = position>>26;
	spot = xtemp | ytemp;
	position += step;
	dest[3] = colormap[source[spot]]; 
		
	count -= 4;
	dest += 4;
    } 
    while (count > 0) 
    { 
	ytemp = position>>4;
	ytemp = ytemp & 4032;
	xtemp = position>>26;
	spot = xtemp | ytemp;
	position += step;
	*dest++ = colormap[source[spot]]; 
	count--;
    } 
} 
#endif


//
// Again..
//
void R_DrawSpanLow (void)
{
#if DOOM_RESOLUTION_SCALE > 1
    R_DrawSampledSpan(1);
#else
    uint32_t position, step;
    uint32_t xtemp, ytemp;
    byte *dest;
    int count;
    int spot;

#ifdef RANGECHECK
    if (ds_x2 < ds_x1
	|| ds_x1<0
	|| ds_x2>=SCREENWIDTH
	|| (unsigned)ds_y>SCREENHEIGHT)
    {
	I_Error( "R_DrawSpan: %i to %i at %i",
		 ds_x1,ds_x2,ds_y);
    }
//	dscount++; 
#endif

    position = (((uint32_t) ds_xfrac << 10) & 0xffff0000)
             | ((ds_yfrac >> 6)  & 0x0000ffff);
    step = (((uint32_t) ds_xstep << 10) & 0xffff0000)
         | ((ds_ystep >> 6)  & 0x0000ffff);

    count = (ds_x2 - ds_x1);

    // Blocky mode, need to multiply by 2.
    ds_x1 <<= 1;
    ds_x2 <<= 1;

    dest = ylookup[ds_y] + columnofs[ds_x1];
#ifdef RIPPLE
    Doom_DrawSpanLow(dest, ds_source, ds_colormap, position, step,
                     (unsigned int)(count + 1));
    return;
#endif

    do
    {
	// Calculate current texture index in u,v.
        ytemp = (position >> 4) & 0x0fc0;
        xtemp = (position >> 26);
        spot = xtemp | ytemp;

	// Lowres/blocky mode does it twice,
	//  while scale is adjusted appropriately.
	*dest++ = ds_colormap[ds_source[spot]];
	*dest++ = ds_colormap[ds_source[spot]];

	position += step;

    } while (count--);
#endif
}

//
// R_InitBuffer 
// Creats lookup tables that avoid
//  multiplies and other hazzles
//  for getting the framebuffer address
//  of a pixel to draw.
//
void
R_InitBuffer
( int		width,
  int		height ) 
{ 
    int		i; 

    /* Preserve the original maximum dimensions while keeping the lookup
       arrays out of the single static-data bank. Resizes reuse the storage. */
    if (width <= 0 || width > MAXWIDTH || height <= 0 || height > MAXHEIGHT)
        I_Error("R_InitBuffer: invalid view dimensions");
    if (ylookup == NULL)
        ylookup = calloc(MAXHEIGHT, sizeof(*ylookup));
    if (columnofs == NULL)
        columnofs = calloc(MAXWIDTH, sizeof(*columnofs));
    if (ylookup == NULL || columnofs == NULL)
        I_Error("R_InitBuffer: out of memory");

    // Handle resize,
    //  e.g. smaller view windows
    //  with border and/or status bar.
    viewwindowx = (SCREENWIDTH-width) >> 1; 

    // Column offset. For windows.
    for (i=0 ; i<width ; i++) 
	columnofs[i] = viewwindowx + i;

    // Samw with base row offset.
    if (width == SCREENWIDTH) 
	viewwindowy = 0; 
    else 
	viewwindowy = (SCREENHEIGHT-SBARHEIGHT-height) >> 1; 

    // Preclaculate all row offsets.
    for (i=0 ; i<height ; i++) 
	ylookup[i] = I_VideoBuffer + (size_t) (i+viewwindowy)*SCREENWIDTH; 
} 
 
 


//
// R_FillBackScreen
// Fills the back screen with a pattern
//  for variable screen sizes
// Also draws a beveled edge.
//
void R_FillBackScreen (void) 
{ 
    byte*	src;
    byte*	dest; 
    int		x;
    int		y; 
    patch_t*	patch;

    // DOOM border patch.
    char       *name1 = DEH_String("FLOOR7_2");

    // DOOM II border patch.
    char *name2 = DEH_String("GRNROCK");

    char *name;

    // If we are running full screen, there is no need to do any of this,
    // and the background buffer can be freed if it was previously in use.

    if (scaledviewwidth == SCREENWIDTH)
    {
        if (background_buffer != NULL)
        {
            Z_Free(background_buffer);
            background_buffer = NULL;
        }

	return;
    }

    // Allocate the background buffer if necessary
	
    if (background_buffer == NULL)
    {
        background_buffer = Z_Malloc((size_t) SCREENWIDTH * (SCREENHEIGHT - SBARHEIGHT),
                                     PU_STATIC, NULL);
    }

    if (gamemode == commercial)
	name = name2;
    else
	name = name1;
    
    src = W_CacheLumpName(name, PU_CACHE); 
    dest = background_buffer;
	 
#if DOOM_RESOLUTION_SCALE > 1
    for (y = 0; y < SCREENHEIGHT - SBARHEIGHT; y += DOOM_RESOLUTION_SCALE)
    {
        byte *row = background_buffer + (size_t)y * SCREENWIDTH;
        byte *tile = src + ((y & 63) << 6);
#ifdef RIPPLE
        for (x = 0; x < SCREENWIDTH; x += 64)
            Doom_CopySamples(row + x, tile, 64 / DOOM_RESOLUTION_SCALE,
                             DOOM_RESOLUTION_SCALE);
#else
        for (x = 0; x < SCREENWIDTH; x += DOOM_RESOLUTION_SCALE)
            row[x] = tile[x & 63];
#endif
    }
#else
    for (y=0 ; y<SCREENHEIGHT-SBARHEIGHT ; y++) 
    { 
	for (x=0 ; x<SCREENWIDTH/64 ; x++) 
	{ 
	    memcpy (dest, src+((y&63)<<6), 64); 
	    dest += 64; 
	} 

	if (SCREENWIDTH&63) 
	{ 
	    memcpy (dest, src+((y&63)<<6), SCREENWIDTH&63); 
	    dest += (SCREENWIDTH&63); 
	} 
    } 
     
#endif

    // Draw screen and bezel; this is done to a separate screen buffer.

    V_UseBuffer(background_buffer);

    patch = W_CachePatchName(DEH_String("brdr_t"),PU_CACHE);

    for (x=0 ; x<scaledviewwidth ; x+=8)
	V_DrawPatch(viewwindowx+x, viewwindowy-8, patch);
    patch = W_CachePatchName(DEH_String("brdr_b"),PU_CACHE);

    for (x=0 ; x<scaledviewwidth ; x+=8)
	V_DrawPatch(viewwindowx+x, viewwindowy+viewheight, patch);
    patch = W_CachePatchName(DEH_String("brdr_l"),PU_CACHE);

    for (y=0 ; y<viewheight ; y+=8)
	V_DrawPatch(viewwindowx-8, viewwindowy+y, patch);
    patch = W_CachePatchName(DEH_String("brdr_r"),PU_CACHE);

    for (y=0 ; y<viewheight ; y+=8)
	V_DrawPatch(viewwindowx+scaledviewwidth, viewwindowy+y, patch);

    // Draw beveled edge. 
    V_DrawPatch(viewwindowx-8,
                viewwindowy-8,
                W_CachePatchName(DEH_String("brdr_tl"),PU_CACHE));
    
    V_DrawPatch(viewwindowx+scaledviewwidth,
                viewwindowy-8,
                W_CachePatchName(DEH_String("brdr_tr"),PU_CACHE));
    
    V_DrawPatch(viewwindowx-8,
                viewwindowy+viewheight,
                W_CachePatchName(DEH_String("brdr_bl"),PU_CACHE));
    
    V_DrawPatch(viewwindowx+scaledviewwidth,
                viewwindowy+viewheight,
                W_CachePatchName(DEH_String("brdr_br"),PU_CACHE));

    V_RestoreBuffer();
} 
 

//
// Copy a screen buffer.
//
void
R_VideoErase
( size_t	ofs,
  size_t	count ) 
{ 
  // LFB copy.
  // This might not be a good idea if memcpy
  //  is not optiomal, e.g. byte by byte on
  //  a 32bit CPU, as GNU GCC/Linux libc did
  //  at one point.

    if (background_buffer != NULL)
    {
#if DOOM_RESOLUTION_SCALE > 1
        size_t end = ofs + count;
        unsigned int y = ofs / SCREENWIDTH;
        size_t row;
        y += (-y) & (DOOM_RESOLUTION_SCALE - 1);
        row = (size_t)y * SCREENWIDTH;
        while (row < end)
        {
            size_t first = ofs > row ? ofs : row;
            size_t rowend = row + SCREENWIDTH;
            unsigned int samples;
            first += (-(unsigned int)(first - row)) & (DOOM_RESOLUTION_SCALE - 1);
            if (rowend > end) rowend = end;
            if (first < rowend)
            {
                samples = (rowend - first + DOOM_RESOLUTION_SCALE - 1) / DOOM_RESOLUTION_SCALE;
#ifdef RIPPLE
                Doom_CopySamples(I_VideoBuffer + first, background_buffer + first,
                                 samples, DOOM_RESOLUTION_SCALE);
#else
                while (samples--)
                {
                    I_VideoBuffer[first] = background_buffer[first];
                    first += DOOM_RESOLUTION_SCALE;
                }
#endif
            }
            row += SCREENWIDTH * DOOM_RESOLUTION_SCALE;
        }
#else
        memcpy(I_VideoBuffer + ofs, background_buffer + ofs, count);
#endif 
    }
} 


//
// R_DrawViewBorder
// Draws the border around the view
//  for different size windows?
//
void R_DrawViewBorder (void) 
{ 
    int		top;
    int		side;
    size_t	ofs;
    int		i; 
 
    if (scaledviewwidth == SCREENWIDTH) 
	return; 
  
    top = ((SCREENHEIGHT-SBARHEIGHT)-viewheight)/2; 
    side = (SCREENWIDTH-scaledviewwidth)/2; 
 
    // copy top and one line of left side 
    R_VideoErase (0, (size_t) top*SCREENWIDTH+side); 
 
    // copy one line of right side and bottom 
    ofs = (size_t) (viewheight+top)*SCREENWIDTH-side; 
    R_VideoErase (ofs, (size_t) top*SCREENWIDTH+side); 
 
    // copy sides using wraparound 
    ofs = (size_t) top*SCREENWIDTH + SCREENWIDTH-side; 
    side <<= 1;
    
    for (i=1 ; i<viewheight ; i++) 
    { 
	R_VideoErase (ofs, side); 
	ofs += SCREENWIDTH; 
    } 

    // ? 
    V_MarkRect (0,0,SCREENWIDTH, SCREENHEIGHT-SBARHEIGHT); 
} 
 
 
