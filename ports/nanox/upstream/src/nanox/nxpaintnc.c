/*
 * Copyright (c) 2000, 2019 Greg Haerr <greg@censoft.com>
 *
 * Nano-X non-client area painting for the built-in window manager
 * (NANOWM).  This file provides the single implementation of
 * nxPaintNCArea(), used by client applications and, in single-process
 * multi-threaded builds (NX_PER_CLIENT_DATA), by the server-side
 * window manager.  In the latter case serv.h renames nxPaintNCArea
 * and the GrXXX calls to the server-side SVR_nxPaintNCArea /
 * SVR_GrXXX symbols, just like the other server files.
 */
#define MWINCLUDECOLORS
#include <stdlib.h>
#if NX_PER_CLIENT_DATA
#include "serv.h"
#endif
#include "nano-X.h"
#include "nanowm.h"
#include "nxdraw.h"

void
nxPaintNCArea(GR_DRAW_ID id, int w, int h, char *title, GR_BOOL active, GR_WM_PROPS props)
{
	int		x = 0;
	int		y = 0;
	GR_GC_ID	gc = GrNewGC();
#if NUKLEARUI
	static GR_FONT_ID fontid = 0;
	if (!fontid)
		fontid = GrCreateFont(GR_FONT_SYSTEM_VAR, 0, NULL);
	GrSetGCFont(gc, fontid);
#endif

	if (props & GR_WM_PROPS_APPFRAME) {
#if NUKLEARUI
		/* draw 1-line black border around window*/
		GrSetGCForeground(gc, GrGetSysColor(GR_COLOR_WINDOWFRAME));
		GrRect(id, gc, x, y, w, h);
		x += 1; y += 1; w -= 2; h -= 2;
#else
		/* draw 2-line 3d border around window*/
		nxDraw3dOutset(id, x, y, w, h);
		x += 2; y += 2; w -= 4; h -= 4;

		/* draw 1-line inset inside border*/
		GrSetGCForeground(gc, GrGetSysColor(GR_COLOR_WINDOWFRAMELT));
		GrRect(id, gc, x, y, w, h);
		x += 1; y += 1; w -= 2; h -= 2;
#endif
	} else if (props & GR_WM_PROPS_BORDER) {
		/* draw 1-line black border around window*/
		GrSetGCForeground(gc, GrGetSysColor(GR_COLOR_WINDOWFRAME));
		GrRect(id, gc, x, y, w, h);
		x += 1; y += 1; w -= 2; h -= 2;
	}

	if (!(props & GR_WM_PROPS_CAPTION))
		goto out;

	/* fill caption*/
	GrSetGCForeground(gc, GrGetSysColor(active?
		GR_COLOR_ACTIVECAPTION: GR_COLOR_INACTIVECAPTION));
	GrFillRect(id, gc, x, y, w, CYCAPTION);

	/* draw caption text*/
	if (title) {
		GrSetGCForeground(gc, GrGetSysColor(active?
			GR_COLOR_ACTIVECAPTIONTEXT: GR_COLOR_INACTIVECAPTIONTEXT));
		GrSetGCUseBackground(gc, GR_FALSE);
#if NUKLEARUI
		/* X = 2 times padding (4)*/
		/* Y = 2 times padding (4) + font ascent+descent (11)*/
		GrText(id, gc, x+2*4, y+2*4+CYTEXTBASE, title, -1, GR_TFASCII|GR_TFBASELINE);
#else
		GrText(id, gc, x+4, y-1, title, -1, GR_TFASCII|GR_TFTOP);
#endif
	}
	y += CYCAPTION;

	/* draw one line under caption*/
	if (props & GR_WM_PROPS_APPFRAME) {
		GrSetGCForeground(gc, GrGetSysColor(GR_COLOR_WINDOWFRAMELT));
		GrLine(id, gc, x, y, x+w-1, y);
	}

	if (props & GR_WM_PROPS_CLOSEBOX) {
#if NUKLEARUI
		GrSetGCForeground(gc, GrGetSysColor(active?
				GR_COLOR_ACTIVECAPTIONTEXT: GR_COLOR_INACTIVECAPTIONTEXT));
		GrSetGCUseBackground(gc, GR_FALSE);
		/* X = width - 3 - "x" width (5) - 2 times padding (4)*/
		/* Y = 2 times padding (4) + font ascent+descent (11)*/
		GrText(id, gc, x+w-3-5-8, y-CYCAPTION+8+CYTEXTBASE, "x", 1,
			GR_TFASCII|GR_TFBASELINE);
#else
		GR_RECT		r;
		/* draw close box*/
		r.x = x + w - CXCLOSEBOX - 2;
		r.y = y - CYCAPTION + 2;
		r.width = CXCLOSEBOX;
		r.height = CYCLOSEBOX;

		nxDraw3dBox(id, r.x, r.y, r.width, r.height,
			GrGetSysColor(GR_COLOR_BTNHIGHLIGHT),
			GrGetSysColor(GR_COLOR_3DFRAME));
		nxInflateRect(&r, -1, -1);
		GrSetGCForeground(gc, GrGetSysColor(GR_COLOR_3DLIGHT));
		GrFillRect(id, gc, r.x, r.y, r.width, r.height);

		nxInflateRect(&r, -1, -1);
		GrSetGCForeground(gc, GrGetSysColor(GR_COLOR_BTNTEXT));
		GrLine(id, gc, r.x, r.y, r.x+r.width-1, r.y+r.height-1);
		GrLine(id, gc, r.x, r.y+r.height-1, r.x+r.width-1, r.y);
#endif
	}

#if 0
	/* fill in client area*/
	y++;
	h -= CYCAPTION+1;
	GrSetGCForeground(gc, GrGetSysColor(GR_COLOR_APPWINDOW));
	GrFillRect(id, gc, x, y, w, h);
#endif

out:
	GrDestroyGC(gc);
}
