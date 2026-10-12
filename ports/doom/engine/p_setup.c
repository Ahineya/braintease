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
//	Do all the WAD I/O, get map description,
//	set up initial state and misc. LUTs.
//



#include <math.h>

#include "z_zone.h"

#include "deh_main.h"
#include "i_swap.h"
#include "m_argv.h"
#include "m_bbox.h"

#include "g_game.h"

#include "i_system.h"
#include "w_wad.h"

#include "doomdef.h"
#include "p_local.h"

#include "s_sound.h"

#include "doomstat.h"
#include "doom_decode.h"


void	P_SpawnMapThing (mapthing_t*	mthing);


//
// MAP related Lookup tables.
// Store VERTEXES, LINEDEFS, SIDEDEFS, etc.
//
int		numvertexes;
vertex_t*	vertexes;

int		numsegs;
seg_t*		segs;

int		numsectors;
sector_t*	sectors;

int		numsubsectors;
subsector_t*	subsectors;

int		numnodes;
node_t*		nodes;

int		numlines;
line_t*		lines;

int		numsides;
side_t*		sides;

static size_t totallines;

// BLOCKMAP
// Created from axis aligned bounding box
// of the map, a rectangular array of
// blocks of size ...
// Used to speed up collision detection
// by spatial subdivision in 2D.
//
// Blockmap size.
int		bmapwidth;
int		bmapheight;	// size in mapblocks
short*		blockmap;	// int for larger maps
// offsets in blockmap are from here
short*		blockmaplump;		
// origin of block map
fixed_t		bmaporgx;
fixed_t		bmaporgy;
// for thing chains
mobj_t**	blocklinks;		


// REJECT
// For fast sight rejection.
// Speeds up enemy AI by skipping detailed
//  LineOf Sight calculation.
// Without special effect, this could be
//  used as a PVS lookup as well.
//
byte*		rejectmatrix;


// Maintain single and multi player starting spots.
#define MAX_DEATHMATCH_STARTS	10

mapthing_t	deathmatchstarts[MAX_DEATHMATCH_STARTS];
mapthing_t*	deathmatch_p;
mapthing_t	playerstarts[MAXPLAYERS];





/* Validate serialized counts before calculating cell allocations. Keeping map
 * indices within signed16 matches the engine's short index fields and Ripple
 * int ABI; unsupported oversized maps fail explicitly instead of wrapping. */
static int P_MapRecordCount(int lump, uint32_t stride, boolean allow_empty)
{
    uint32_t length = (uint32_t)W_LumpLength(lump);
    uint32_t count;
    if (length % stride)
        I_Error("Map lump has an incomplete serialized record");
    count = length / stride;
    if ((!count && !allow_empty) || count > INT16_MAX)
        I_Error("Map lump record count is outside the supported index range");
    return (int)count;
}

static void *P_MapAllocate(size_t count, size_t size)
{
    void *result;
    size_t bytes;
    if (!count)
        return NULL;
    if (size > (size_t)-1 / count)
        I_Error("Map allocation size overflow");
    bytes = count * size;
    result = Z_Malloc(bytes, PU_LEVEL, NULL);
    memset(result, 0, bytes);
    return result;
}

/* Explicit traversal prevents a malformed cyclic BSP from overflowing the
 * guest stack later. Node numbers need not be stored in postorder. */
static void P_ValidateNodes(void)
{
    byte *mark;
    byte *next_child;
    int *stack;
    int root, depth, node;
    uint16_t child;
    if (!numnodes)
        return;
    mark = Z_Malloc((size_t)numnodes, PU_STATIC, NULL);
    next_child = Z_Malloc((size_t)numnodes, PU_STATIC, NULL);
    stack = Z_Malloc((size_t)numnodes * sizeof(*stack), PU_STATIC, NULL);
    memset(mark, 0, (size_t)numnodes);
    memset(next_child, 0, (size_t)numnodes);
    for (root = 0; root < numnodes; ++root)
    {
        if (mark[root])
            continue;
        depth = 1;
        stack[0] = root;
        mark[root] = 1;
        while (depth)
        {
            node = stack[depth - 1];
            if (next_child[node] == 2)
            {
                mark[node] = 2;
                --depth;
                continue;
            }
            child = nodes[node].children[next_child[node]++];
            if (child & NF_SUBSECTOR)
                continue;
            if (mark[child] == 1)
                I_Error("NODES: cyclic BSP reference");
            if (!mark[child])
            {
                if (depth >= numnodes)
                    I_Error("NODES: invalid BSP depth");
                stack[depth++] = (int)child;
                mark[child] = 1;
            }
        }
    }
    Z_Free(stack);
    Z_Free(next_child);
    Z_Free(mark);
}

//
// P_LoadVertexes
//
void P_LoadVertexes(int lump)
{
    const byte *data;
    const byte *record;
    int i;
    numvertexes = P_MapRecordCount(lump, MAP_VERTEX_OCTETS, false);
    vertexes = P_MapAllocate(numvertexes, sizeof(*vertexes));
    data = W_CacheLumpNum(lump, PU_STATIC);
    for (i = 0; i < numvertexes; ++i)
    {
        record = data + (size_t)i * MAP_VERTEX_OCTETS;
        vertexes[i].x = DoomReadFixed16(record);
        vertexes[i].y = DoomReadFixed16(record + 2);
    }
    W_ReleaseLumpNum(lump);
}

//
// GetSectorAtNullAddress
//
sector_t* GetSectorAtNullAddress(void)
{
    static boolean null_sector_is_initialized = false;
    static sector_t null_sector;

    if (!null_sector_is_initialized)
    {
        memset(&null_sector, 0, sizeof(null_sector));
        I_GetMemoryValue(0, &null_sector.floorheight, 4);
        I_GetMemoryValue(4, &null_sector.ceilingheight, 4);
        null_sector_is_initialized = true;
    }

    return &null_sector;
}

//
// P_LoadSegs
//
void P_LoadSegs(int lump)
{
    const byte *data;
    const byte *record;
    seg_t *seg;
    line_t *line;
    uint16_t v1, v2, line_index, side;
    int side_index;
    int i;
    numsegs = P_MapRecordCount(lump, MAP_SEG_OCTETS, false);
    segs = P_MapAllocate(numsegs, sizeof(*segs));
    data = W_CacheLumpNum(lump, PU_STATIC);
    for (i = 0; i < numsegs; ++i)
    {
        record = data + (size_t)i * MAP_SEG_OCTETS;
        v1 = DoomReadLE16(record);
        v2 = DoomReadLE16(record + 2);
        line_index = DoomReadLE16(record + 6);
        side = DoomReadLE16(record + 8);
        if (v1 >= (uint16_t)numvertexes || v2 >= (uint16_t)numvertexes
         || line_index >= (uint16_t)numlines || side > 1)
            I_Error("SEGS: invalid vertex, linedef or side reference");
        line = &lines[line_index];
        side_index = line->sidenum[side];
        if (side_index < 0 || side_index >= numsides)
            I_Error("SEGS: missing front sidedef");
        seg = &segs[i];
        seg->v1 = &vertexes[v1];
        seg->v2 = &vertexes[v2];
        seg->angle = (uint32_t)DoomReadLE16(record + 4) << 16;
        seg->offset = DoomReadFixed16(record + 10);
        seg->linedef = line;
        seg->sidedef = &sides[side_index];
        seg->frontsector = sides[side_index].sector;
        if (line->flags & ML_TWOSIDED)
        {
            side_index = line->sidenum[side ^ 1];
            /* Preserve the upstream missing-back-sector compatibility case. */
            seg->backsector = side_index < 0
                            ? GetSectorAtNullAddress() : sides[side_index].sector;
        }
        else
            seg->backsector = NULL;
    }
    W_ReleaseLumpNum(lump);
}


//
// P_LoadSubsectors
//
void P_LoadSubsectors(int lump)
{
    const byte *data;
    const byte *record;
    uint16_t count, first;
    int i;
    numsubsectors = P_MapRecordCount(lump, MAP_SUBSECTOR_OCTETS, false);
    subsectors = P_MapAllocate(numsubsectors, sizeof(*subsectors));
    data = W_CacheLumpNum(lump, PU_STATIC);
    for (i = 0; i < numsubsectors; ++i)
    {
        record = data + (size_t)i * MAP_SUBSECTOR_OCTETS;
        count = DoomReadLE16(record);
        first = DoomReadLE16(record + 2);
        if (!count || first >= (uint16_t)numsegs
         || (uint32_t)count > (uint32_t)numsegs - first)
            I_Error("SSECTORS: invalid seg range");
        subsectors[i].numlines = (short)count;
        subsectors[i].firstline = (short)first;
    }
    W_ReleaseLumpNum(lump);
}



//
// P_LoadSectors
//
void P_LoadSectors(int lump)
{
    const byte *data;
    const byte *record;
    sector_t *sector;
    char name[9];
    int i;
    numsectors = P_MapRecordCount(lump, MAP_SECTOR_OCTETS, false);
    sectors = P_MapAllocate(numsectors, sizeof(*sectors));
    data = W_CacheLumpNum(lump, PU_STATIC);
    for (i = 0; i < numsectors; ++i)
    {
        record = data + (size_t)i * MAP_SECTOR_OCTETS;
        sector = &sectors[i];
        sector->floorheight = DoomReadFixed16(record);
        sector->ceilingheight = DoomReadFixed16(record + 2);
        DoomReadName8(record + 4, name);
        sector->floorpic = R_FlatNumForName(name);
        DoomReadName8(record + 12, name);
        sector->ceilingpic = R_FlatNumForName(name);
        sector->lightlevel = (short)DoomReadS16(record + 20);
        sector->special = (short)DoomReadS16(record + 22);
        sector->tag = (short)DoomReadS16(record + 24);
    }
    W_ReleaseLumpNum(lump);
}


//
// P_LoadNodes
//
void P_LoadNodes(int lump)
{
    const byte *data;
    const byte *record;
    node_t *node;
    uint16_t child;
    int i, j, k;
    numnodes = P_MapRecordCount(lump, MAP_NODE_OCTETS, true);
    nodes = P_MapAllocate(numnodes, sizeof(*nodes));
    if (numnodes == 0)
    {
        if (numsubsectors != 1)
            I_Error("NODES: empty BSP requires exactly one subsector");
        return;
    }
    data = W_CacheLumpNum(lump, PU_STATIC);
    for (i = 0; i < numnodes; ++i)
    {
        record = data + (size_t)i * MAP_NODE_OCTETS;
        node = &nodes[i];
        node->x = DoomReadFixed16(record);
        node->y = DoomReadFixed16(record + 2);
        node->dx = DoomReadFixed16(record + 4);
        node->dy = DoomReadFixed16(record + 6);
        for (j = 0; j < 2; ++j)
        {
            for (k = 0; k < 4; ++k)
                node->bbox[j][k] = DoomReadFixed16(record + 8 + j * 8 + k * 2);
            child = DoomReadLE16(record + 24 + j * 2);
            if (child & NF_SUBSECTOR)
            {
                if ((child & UINT16_C(0x7fff)) >= (uint16_t)numsubsectors)
                    I_Error("NODES: invalid subsector reference");
            }
            else if (child >= (uint16_t)numnodes)
                I_Error("NODES: invalid child node reference");
            node->children[j] = child;
        }
    }
    W_ReleaseLumpNum(lump);
    P_ValidateNodes();
}


//
// P_LoadThings
//
void P_LoadThings(int lump)
{
    const byte *data;
    const byte *record;
    mapthing_t thing;
    int count, i;
    boolean spawn;
    count = P_MapRecordCount(lump, MAP_THING_OCTETS, false);
    data = W_CacheLumpNum(lump, PU_STATIC);
    for (i = 0; i < count; ++i)
    {
        record = data + (size_t)i * MAP_THING_OCTETS;
        thing.x = (short)DoomReadS16(record);
        thing.y = (short)DoomReadS16(record + 2);
        thing.angle = (short)DoomReadS16(record + 4);
        thing.type = (short)DoomReadS16(record + 6);
        thing.options = (short)DoomReadS16(record + 8);
        spawn = true;
        if (gamemode != commercial)
        {
            switch (thing.type)
            {
                case 68: case 64: case 88: case 89: case 69:
                case 67: case 71: case 65: case 66: case 84:
                    spawn = false;
                    break;
            }
        }
        /* Match vanilla's shareware/commercial filtering and spawn ordering. */
        if (!spawn)
            break;
        P_SpawnMapThing(&thing);
    }
    W_ReleaseLumpNum(lump);
}


//
// P_LoadLineDefs
// Also counts secret lines for intermissions.
//
void P_LoadLineDefs(int lump)
{
    const byte *data;
    const byte *record;
    line_t *line;
    vertex_t *v1, *v2;
    uint16_t vertex1, vertex2, side;
    int i, j;
    numlines = P_MapRecordCount(lump, MAP_LINE_OCTETS, false);
    lines = P_MapAllocate(numlines, sizeof(*lines));
    data = W_CacheLumpNum(lump, PU_STATIC);
    for (i = 0; i < numlines; ++i)
    {
        record = data + (size_t)i * MAP_LINE_OCTETS;
        vertex1 = DoomReadLE16(record);
        vertex2 = DoomReadLE16(record + 2);
        if (vertex1 >= (uint16_t)numvertexes || vertex2 >= (uint16_t)numvertexes)
            I_Error("LINEDEFS: invalid vertex reference");
        line = &lines[i];
        line->flags = (short)DoomReadS16(record + 4);
        line->special = (short)DoomReadS16(record + 6);
        line->tag = (short)DoomReadS16(record + 8);
        v1 = line->v1 = &vertexes[vertex1];
        v2 = line->v2 = &vertexes[vertex2];
        line->dx = v2->x - v1->x;
        line->dy = v2->y - v1->y;
        if (!line->dx)
            line->slopetype = ST_VERTICAL;
        else if (!line->dy)
            line->slopetype = ST_HORIZONTAL;
        else
            line->slopetype = FixedDiv(line->dy, line->dx) > 0
                            ? ST_POSITIVE : ST_NEGATIVE;
        line->bbox[BOXLEFT] = v1->x < v2->x ? v1->x : v2->x;
        line->bbox[BOXRIGHT] = v1->x > v2->x ? v1->x : v2->x;
        line->bbox[BOXBOTTOM] = v1->y < v2->y ? v1->y : v2->y;
        line->bbox[BOXTOP] = v1->y > v2->y ? v1->y : v2->y;
        for (j = 0; j < 2; ++j)
        {
            side = DoomReadLE16(record + 10 + j * 2);
            if (side == UINT16_C(0xffff))
                line->sidenum[j] = -1;
            else if (side >= (uint16_t)numsides)
                I_Error("LINEDEFS: invalid sidedef reference");
            else
                line->sidenum[j] = (short)side;
        }
        if (line->sidenum[0] < 0)
            I_Error("LINEDEFS: missing front sidedef");
        line->frontsector = sides[line->sidenum[0]].sector;
        line->backsector = line->sidenum[1] < 0
                         ? NULL : sides[line->sidenum[1]].sector;
    }
    W_ReleaseLumpNum(lump);
}


//
// P_LoadSideDefs
//
void P_LoadSideDefs(int lump)
{
    const byte *data;
    const byte *record;
    side_t *side;
    char name[9];
    uint16_t sector;
    int i;
    numsides = P_MapRecordCount(lump, MAP_SIDE_OCTETS, false);
    sides = P_MapAllocate(numsides, sizeof(*sides));
    data = W_CacheLumpNum(lump, PU_STATIC);
    for (i = 0; i < numsides; ++i)
    {
        record = data + (size_t)i * MAP_SIDE_OCTETS;
        sector = DoomReadLE16(record + 28);
        if (sector >= (uint16_t)numsectors)
            I_Error("SIDEDEFS: invalid sector reference");
        side = &sides[i];
        side->textureoffset = DoomReadFixed16(record);
        side->rowoffset = DoomReadFixed16(record + 2);
        DoomReadName8(record + 4, name);
        side->toptexture = R_TextureNumForName(name);
        DoomReadName8(record + 12, name);
        side->bottomtexture = R_TextureNumForName(name);
        DoomReadName8(record + 20, name);
        side->midtexture = R_TextureNumForName(name);
        side->sector = &sectors[sector];
    }
    W_ReleaseLumpNum(lump);
}


//
// P_LoadBlockMap
//
void P_LoadBlockMap(int lump)
{
    const byte *data;
    uint32_t length, words, i, cells, cursor;
    uint16_t value;
    size_t link_bytes;
    length = (uint32_t)W_LumpLength(lump);
    if (length < 8 || length % 2)
        I_Error("BLOCKMAP: truncated header or word");
    words = length / 2;
    data = W_CacheLumpNum(lump, PU_STATIC);
    blockmaplump = P_MapAllocate(words, sizeof(*blockmaplump));
    for (i = 0; i < words; ++i)
        blockmaplump[i] = (short)DoomReadS16(data + i * 2);
    bmaporgx = DoomReadFixed16(data);
    bmaporgy = DoomReadFixed16(data + 2);
    value = DoomReadLE16(data + 4);
    if (!value || value > INT16_MAX)
        I_Error("BLOCKMAP: invalid width");
    bmapwidth = (int)value;
    value = DoomReadLE16(data + 6);
    if (!value || value > INT16_MAX)
        I_Error("BLOCKMAP: invalid height");
    bmapheight = (int)value;
    cells = (uint32_t)bmapwidth * (uint32_t)bmapheight;
    if (cells > INT16_MAX || cells > words - 4)
        I_Error("BLOCKMAP: invalid offset table size");
    blockmap = blockmaplump + 4;
    for (i = 0; i < cells; ++i)
    {
        cursor = (uint16_t)blockmap[i];
        if (cursor < 4 + cells || cursor >= words || cursor > INT16_MAX)
            I_Error("BLOCKMAP: invalid list offset");
        for (;; ++cursor)
        {
            if (cursor >= words)
                I_Error("BLOCKMAP: unterminated line list");
            value = (uint16_t)blockmaplump[cursor];
            if (value == UINT16_C(0xffff))
                break;
            if (value >= (uint16_t)numlines)
                I_Error("BLOCKMAP: invalid linedef reference");
        }
    }
    W_ReleaseLumpNum(lump);
    link_bytes = (size_t)cells * sizeof(*blocklinks);
    blocklinks = Z_Malloc(link_bytes, PU_LEVEL, NULL);
    memset(blocklinks, 0, link_bytes);
}



//
// P_GroupLines
// Builds sector line lists and subsector sector numbers.
// Finds block bounding boxes for sectors.
//
void P_GroupLines (void)
{
    line_t**		linebuffer;
    int			i;
    int			j;
    line_t*		li;
    sector_t*		sector;
    subsector_t*	ss;
    seg_t*		seg;
    fixed_t		bbox[4];
    int			block;
	
    // look up sector number for each subsector
    ss = subsectors;
    for (i=0 ; i<numsubsectors ; i++, ss++)
    {
	seg = &segs[ss->firstline];
	ss->sector = seg->sidedef->sector;
    }

    // count number of lines in each sector
    li = lines;
    totallines = 0;
    for (i=0 ; i<numlines ; i++, li++)
    {
	totallines++;
	li->frontsector->linecount++;

	if (li->backsector && li->backsector != li->frontsector)
	{
	    li->backsector->linecount++;
	    totallines++;
	}
    }

    // build line tables for each sector	
    linebuffer = Z_Malloc (totallines*sizeof(line_t *), PU_LEVEL, 0);

    for (i=0; i<numsectors; ++i)
    {
        // Assign the line buffer for this sector

        sectors[i].lines = linebuffer;
        linebuffer += sectors[i].linecount;

        // Reset linecount to zero so in the next stage we can count
        // lines into the list.

        sectors[i].linecount = 0;
    }

    // Assign lines to sectors

    for (i=0; i<numlines; ++i)
    { 
        li = &lines[i];

        if (li->frontsector != NULL)
        {
            sector = li->frontsector;

            sector->lines[sector->linecount] = li;
            ++sector->linecount;
        }

        if (li->backsector != NULL && li->frontsector != li->backsector)
        {
            sector = li->backsector;

            sector->lines[sector->linecount] = li;
            ++sector->linecount;
        }
    }
    
    // Generate bounding boxes for sectors
	
    sector = sectors;
    for (i=0 ; i<numsectors ; i++, sector++)
    {
	M_ClearBox (bbox);

	for (j=0 ; j<sector->linecount; j++)
	{
            li = sector->lines[j];

            M_AddToBox (bbox, li->v1->x, li->v1->y);
            M_AddToBox (bbox, li->v2->x, li->v2->y);
	}

	// set the degenmobj_t to the middle of the bounding box
	sector->soundorg.x = (bbox[BOXRIGHT]+bbox[BOXLEFT])/2;
	sector->soundorg.y = (bbox[BOXTOP]+bbox[BOXBOTTOM])/2;
		
	// adjust bounding box to map blocks
	block = (bbox[BOXTOP]-bmaporgy+MAXRADIUS)>>MAPBLOCKSHIFT;
	block = block >= bmapheight ? bmapheight-1 : block;
	sector->blockbox[BOXTOP]=block;

	block = (bbox[BOXBOTTOM]-bmaporgy-MAXRADIUS)>>MAPBLOCKSHIFT;
	block = block < 0 ? 0 : block;
	sector->blockbox[BOXBOTTOM]=block;

	block = (bbox[BOXRIGHT]-bmaporgx+MAXRADIUS)>>MAPBLOCKSHIFT;
	block = block >= bmapwidth ? bmapwidth-1 : block;
	sector->blockbox[BOXRIGHT]=block;

	block = (bbox[BOXLEFT]-bmaporgx-MAXRADIUS)>>MAPBLOCKSHIFT;
	block = block < 0 ? 0 : block;
	sector->blockbox[BOXLEFT]=block;
    }
	
}

// Pad the REJECT lump with extra data when the lump is too small,
// to simulate a REJECT buffer overflow in Vanilla Doom.

static void PadRejectArray(byte *array, uint32_t length)
{
    uint32_t i;
    uint32_t pad[4];
    byte fill;
    /* These are four serialized 32-bit vanilla allocator values: 16 octets,
     * independent of CHAR_BIT, sizeof(unsigned int), or target pointer size. */
    pad[0] = (((uint32_t)totallines * 4 + 3) & ~UINT32_C(3)) + 24;
    pad[1] = 0;
    pad[2] = 50;
    pad[3] = UINT32_C(0x1d4a11);
    for (i = 0; i < length && i < 16; ++i)
        array[i] = (byte)((pad[i / 4] >> ((i % 4) * 8)) & 255U);
    if (length > 16)
    {
        fprintf(stderr, "PadRejectArray: REJECT lump too short to pad! (%lu > 16)\n",
                (unsigned long)length);
        fill = M_CheckParm("-reject_pad_with_ff") ? 255 : 0;
        memset(array + 16, fill, (size_t)(length - 16));
    }
}

static void P_LoadReject(int lumpnum)
{
    uint32_t minimum, length;
    minimum = ((uint32_t)numsectors * (uint32_t)numsectors + 7) / 8;
    length = (uint32_t)W_LumpLength(lumpnum);
    if (length >= minimum)
        rejectmatrix = W_CacheLumpNum(lumpnum, PU_LEVEL);
    else
    {
        rejectmatrix = Z_Malloc((size_t)minimum, PU_LEVEL, &rejectmatrix);
        if (length)
            W_ReadLump(lumpnum, rejectmatrix);
        PadRejectArray(rejectmatrix + length, minimum - length);
    }
}

//
// P_SetupLevel
//
void
P_SetupLevel
( int		episode,
  int		map,
  int		playermask,
  skill_t	skill)
{
    int		i;
    char	lumpname[9];
    int		lumpnum;
	
    totalkills = totalitems = totalsecret = wminfo.maxfrags = 0;
    wminfo.partime = 180;
    for (i=0 ; i<MAXPLAYERS ; i++)
    {
	players[i].killcount = players[i].secretcount 
	    = players[i].itemcount = 0;
    }

    // Initial height of PointOfView
    // will be set by player think.
    players[consoleplayer].viewz = 1; 

    // Make sure all sounds are stopped before Z_FreeTags.
    S_Start ();			

    Z_FreeTags (PU_LEVEL, PU_PURGELEVEL-1);

    // UNUSED W_Profile ();
    P_InitThinkers ();
	   
    // find map name
    if ( gamemode == commercial)
    {
	if (map<10)
	    DEH_snprintf(lumpname, 9, "map0%i", map);
	else
	    DEH_snprintf(lumpname, 9, "map%i", map);
    }
    else
    {
	lumpname[0] = 'E';
	lumpname[1] = '0' + episode;
	lumpname[2] = 'M';
	lumpname[3] = '0' + map;
	lumpname[4] = 0;
    }

    lumpnum = W_GetNumForName (lumpname);
	
    leveltime = 0;
	
    // note: most of this ordering is important	
    P_LoadVertexes (lumpnum+ML_VERTEXES);
    P_LoadSectors (lumpnum+ML_SECTORS);
    P_LoadSideDefs (lumpnum+ML_SIDEDEFS);
    P_LoadLineDefs (lumpnum+ML_LINEDEFS);
    /* Load referenced arrays first so malformed indices are rejected while
     * decoding, before any renderer or block iterator can dereference them. */
    P_LoadBlockMap (lumpnum+ML_BLOCKMAP);
    P_LoadSegs (lumpnum+ML_SEGS);
    P_LoadSubsectors (lumpnum+ML_SSECTORS);
    P_LoadNodes (lumpnum+ML_NODES);

    P_GroupLines ();
    P_LoadReject (lumpnum+ML_REJECT);

    bodyqueslot = 0;
    deathmatch_p = deathmatchstarts;
    P_LoadThings (lumpnum+ML_THINGS);
    
    // if deathmatch, randomly spawn the active players
    if (deathmatch)
    {
	for (i=0 ; i<MAXPLAYERS ; i++)
	    if (playeringame[i])
	    {
		players[i].mo = NULL;
		G_DeathMatchSpawnPlayer (i);
	    }
			
    }

    // clear special respawning que
    iquehead = iquetail = 0;		
	
    // set up world state
    P_SpawnSpecials ();
	
    // build subsector connect matrix
    //	UNUSED P_ConnectSubsectors ();

    // preload graphics
    if (precache)
	R_PrecacheLevel ();

    //printf ("free memory: 0x%x\n", Z_FreeMemory());

}



//
// P_Init
//
void P_Init (void)
{
    P_InitSwitchList ();
    P_InitPicAnims ();
    R_InitSprites (sprnames);
}



