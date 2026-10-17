/*
 * Copyright (c) 2000, 2019 Greg Haerr <greg@censoft.com>
 *
 * Nano-X Draw Library
 */
#define MWINCLUDECOLORS
#include <stdlib.h>
#include "nano-X.h"
#include "nanowm.h"
#include "nxdraw.h"

/*
 * GrGetSystemColor color scheme definitions
 */ 
#if SCHEME_NUKLEAR
const GR_COLOR nxSysColors[MAXSYSCOLORS] = {
	/* desktop background*/
	GR_RGB(  0, 128, 128),  /* GR_COLOR_DESKTOP             */

	/* caption colors*/
	GR_RGB( 40,  40,  40),	/* GR_COLOR_ACTIVECAPTION       */
	GR_RGB(175, 175, 175),  /* GR_COLOR_ACTIVECAPTIONTEXT   */
	GR_RGB( 35,  35,  35),	/* GR_COLOR_INACTIVECAPTION     */
	GR_RGB(175, 175, 175),  /* GR_COLOR_INACTIVECAPTIONTEXT */

	/* 3d border shades (UNUSED in Nuklear)*/
	GR_RGB(  0,   0,   0),  /* GR_COLOR_3DFRAME             */
	GR_RGB(162, 141, 104),	/* GR_COLOR_BTNSHADOW           */
	GR_RGB(213, 204, 187),	/* GR_COLOR_3DLIGHT             */
	GR_RGB(234, 230, 221), 	/* GR_COLOR_BTNHIGHLIGHT        */

	/* top level application window backgrounds/text (FIXME REMOVE?)*/
	GR_RGB( 45,  45,  45),	/* GR_COLOR_APPWINDOW           */
	GR_RGB(175, 175, 175),  /* GR_COLOR_APPTEXT             */

	/* button control backgrounds/text (usually same as app window colors)*/
	GR_RGB( 50,  50,  50),	/* GR_COLOR_BTNFACE             */
	GR_RGB(175, 175, 175),  /* GR_COLOR_BTNTEXT             */

	/* edit/listbox control backgrounds/text, selected highlights*/
	GR_RGB( 45,  45,  45),  /* GR_COLOR_WINDOW              */
	GR_RGB(175, 175, 175),  /* GR_COLOR_WINDOWTEXT          */
	GR_RGB(128,   0,   0),  /* GR_COLOR_HIGHLIGHT           */
	GR_RGB(175, 175, 175),  /* GR_COLOR_HIGHLIGHTTEXT       */
	GR_RGB(175, 175, 175),  /* GR_COLOR_GRAYTEXT            */

	/* menu backgrounds/text*/
	GR_RGB( 40,  40,  40),	/* GR_COLOR_MENU                */
	GR_RGB(175, 175, 175),  /* GR_COLOR_MENUTEXT            */

	/* window border and interior line under caption*/
	GR_RGB( 65,  65,  65),	/* GR_COLOR_WINDOWFRAME         */
	GR_RGB( 65,  65,  65)   /* GR_COLOR_WINDOWFRAMELT       */
};
#endif

#if SCHEME_NUK16
const GR_COLOR nxSysColors[MAXSYSCOLORS] = {
	/* desktop background*/
	//GR_RGB(  0, 127, 127),  /* GR_COLOR_DESKTOP             */
	GR_RGB(128, 128, 128),  /* GR_COLOR_DESKTOP             */

	/* caption colors*/
	GR_RGB(127, 127, 127),	/* GR_COLOR_ACTIVECAPTION       */
	GR_RGB(175, 175, 175),  /* GR_COLOR_ACTIVECAPTIONTEXT   */
	GR_RGB( 35,  35,  35),	/* GR_COLOR_INACTIVECAPTION     */
	GR_RGB(175, 175, 175),  /* GR_COLOR_INACTIVECAPTIONTEXT */

	/* 3d border shades (UNUSED in Nuklear)*/
	GR_RGB(  0,   0,   0),  /* GR_COLOR_3DFRAME             */
	GR_RGB(162, 141, 104),	/* GR_COLOR_BTNSHADOW           */
	GR_RGB(213, 204, 187),	/* GR_COLOR_3DLIGHT             */
	GR_RGB(234, 230, 221), 	/* GR_COLOR_BTNHIGHLIGHT        */

	/* top level application window backgrounds/text (FIXME REMOVE?)*/
	GR_RGB( 45,  45,  45),	/* GR_COLOR_APPWINDOW           */
	GR_RGB(175, 175, 175),  /* GR_COLOR_APPTEXT             */

	/* button control backgrounds/text (usually same as app window colors)*/
	GR_RGB( 50,  50,  50),	/* GR_COLOR_BTNFACE             */
	GR_RGB(175, 175, 175),  /* GR_COLOR_BTNTEXT             */

	/* edit/listbox control backgrounds/text, selected highlights*/
	GR_RGB( 45,  45,  45),  /* GR_COLOR_WINDOW              */
	GR_RGB(175, 175, 175),  /* GR_COLOR_WINDOWTEXT          */
	GR_RGB(128,   0,   0),  /* GR_COLOR_HIGHLIGHT           */
	GR_RGB(175, 175, 175),  /* GR_COLOR_HIGHLIGHTTEXT       */
	GR_RGB(175, 175, 175),  /* GR_COLOR_GRAYTEXT            */

	/* menu backgrounds/text*/
	GR_RGB( 40,  40,  40),	/* GR_COLOR_MENU                */
	GR_RGB(175, 175, 175),  /* GR_COLOR_MENUTEXT            */

	/* window border and interior line under caption*/
	GR_RGB( 32,  32,  32),	/* GR_COLOR_WINDOWFRAME         */
	GR_RGB( 65,  65,  65)   /* GR_COLOR_WINDOWFRAMELT       */
};
#endif

#ifdef SCHEME_TAN
const GR_COLOR nxSysColors[MAXSYSCOLORS] = {
	/* desktop background*/
	GR_RGB(  0, 128, 128),  /* GR_COLOR_DESKTOP             */

	/* caption colors*/
	GR_RGB(128,   0,   0),	/* GR_COLOR_ACTIVECAPTION       */
	GR_RGB(255, 255, 255),  /* GR_COLOR_ACTIVECAPTIONTEXT   */
	GR_RGB(162, 141, 104),	/* GR_COLOR_INACTIVECAPTION     */
	GR_RGB(192, 192, 192),  /* GR_COLOR_INACTIVECAPTIONTEXT */

	/* 3d border shades*/
	GR_RGB(  0,   0,   0),  /* GR_COLOR_3DFRAME             */
	GR_RGB(162, 141, 104),	/* GR_COLOR_BTNSHADOW           */
	GR_RGB(213, 204, 187),	/* GR_COLOR_3DLIGHT             */
	GR_RGB(234, 230, 221), 	/* GR_COLOR_BTNHIGHLIGHT        */

	/* top level application window backgrounds/text*/
	GR_RGB(255, 255, 255),	/* GR_COLOR_APPWINDOW           */
	GR_RGB(  0,   0,   0),  /* GR_COLOR_APPTEXT             */

	/* button control backgrounds/text (usually same as app window colors)*/
	GR_RGB(213, 204, 187),	/* GR_COLOR_BTNFACE             */
	GR_RGB(  0,   0,   0),  /* GR_COLOR_BTNTEXT             */

	/* edit/listbox control backgrounds/text, selected highlights*/
	GR_RGB(255, 255, 255),  /* GR_COLOR_WINDOW              */
	GR_RGB(  0,   0,   0),  /* GR_COLOR_WINDOWTEXT          */
	GR_RGB(128,   0,   0),  /* GR_COLOR_HIGHLIGHT           */
	GR_RGB(255, 255, 255),  /* GR_COLOR_HIGHLIGHTTEXT       */
	GR_RGB( 64,  64,  64),  /* GR_COLOR_GRAYTEXT            */

	/* menu backgrounds/text*/
	GR_RGB(213, 204, 187),	/* GR_COLOR_MENU                */
	GR_RGB(  0,   0,   0),  /* GR_COLOR_MENUTEXT            */

	/* window border and interior line under caption*/
	GR_RGB(  0,   0,   0),  /* GR_COLOR_WINDOWFRAME         */
	GR_RGB(213, 204, 187)	/* GR_COLOR_WINDOWFRAMELT       */
};
#endif

#ifdef SCHEME_WINSTD
const GR_COLOR nxSysColors[MAXSYSCOLORS] = {
	/* desktop background*/
	GR_RGB(  0, 128, 128),  /* GR_COLOR_DESKTOP             */

	/* caption colors*/
	GR_RGB(128,   0, 128),	/* GR_COLOR_ACTIVECAPTION       */
	GR_RGB(255, 255, 255),  /* GR_COLOR_ACTIVECAPTIONTEXT   */
	GR_RGB(128, 128, 128), 	/* GR_COLOR_INACTIVECAPTION     */
	GR_RGB(192, 192, 192),  /* GR_COLOR_INACTIVECAPTIONTEXT */

	/* 3d border shades*/
	GR_RGB(  0,   0,   0),  /* GR_COLOR_3DFRAME             */
	GR_RGB(128, 128, 128),	/* GR_COLOR_BTNSHADOW           */
	GR_RGB(223, 223, 223),	/* GR_COLOR_3DLIGHT             */
	GR_RGB(255, 255, 255), 	/* GR_COLOR_BTNHIGHLIGHT        */

	/* top level application window backgrounds/text*/
	GR_RGB(192, 192, 192),	/* GR_COLOR_APPWINDOW           */
	GR_RGB(  0,   0,   0),  /* GR_COLOR_APPTEXT             */

	/* button control backgrounds/text (usually same as app window colors)*/
	GR_RGB(192, 192, 192),	/* GR_COLOR_BTNFACE             */
	GR_RGB(  0,   0,   0),  /* GR_COLOR_BTNTEXT             */

	/* edit/listbox control backgrounds/text, selected highlights*/
	GR_RGB(255, 255, 255),  /* GR_COLOR_WINDOW              */
	GR_RGB(  0,   0,   0),  /* GR_COLOR_WINDOWTEXT          */
	GR_RGB(128,   0,   0),  /* GR_COLOR_HIGHLIGHT           */
	GR_RGB(255, 255, 255),  /* GR_COLOR_HIGHLIGHTTEXT       */
	GR_RGB( 64,  64,  64),  /* GR_COLOR_GRAYTEXT            */

	/* menu backgrounds/text*/
	GR_RGB(192, 192, 192),	/* GR_COLOR_MENU                */
	GR_RGB(  0,   0,   0),  /* GR_COLOR_MENUTEXT            */

	/* window border and interior line under caption*/
	GR_RGB(  0,   0,   0),  /* GR_COLOR_WINDOWFRAME         */
	GR_RGB(192, 192, 192)	/* GR_COLOR_WINDOWFRAMELT       */
};
#endif

#ifdef SCHEME_OLD
static const GR_COLOR sysColors[MAXSYSCOLORS] = {
	/* desktop background*/
	GR_RGB(  0, 128, 128),  /* GR_COLOR_DESKTOP             */

	/* caption colors*/
	GR_RGB(128,   0, 128),	/* GR_COLOR_ACTIVECAPTION       */
	GR_RGB(255, 255, 255),  /* GR_COLOR_ACTIVECAPTIONTEXT   */
	GR_RGB(  0,  64, 128),	/* GR_COLOR_INACTIVECAPTION     */
	GR_RGB(192, 192, 192),  /* GR_COLOR_INACTIVECAPTIONTEXT */

	/* 3d border shades*/
	GR_RGB(  0,   0,   0),  /* GR_COLOR_3DFRAME         */
	GR_RGB(128, 128, 128),	/* GR_COLOR_BTNSHADOW           */
	GR_RGB(192, 192, 192),	/* GR_COLOR_3DLIGHT             */
	GR_RGB(223, 223, 223), 	/* GR_COLOR_BTNHIGHLIGHT        */

	/* top level application window backgrounds/text*/
	GR_RGB(160, 160, 160),	/* GR_COLOR_APPWINDOW           */
	GR_RGB(  0,   0,   0),  /* GR_COLOR_APPTEXT             */

	/* button control backgrounds/text (usually same as app window colors)*/
	GR_RGB(160, 160, 160),	/* GR_COLOR_BTNFACE             */
	GR_RGB(  0,   0,   0),  /* GR_COLOR_BTNTEXT             */

	/* edit/listbox control backgrounds/text, selected highlights*/
	GR_RGB(255, 255, 255),  /* GR_COLOR_WINDOW              */
	GR_RGB(  0,   0,   0),  /* GR_COLOR_WINDOWTEXT          */
	GR_RGB(128,   0,   0),  /* GR_COLOR_HIGHLIGHT           */
	GR_RGB(255, 255, 255),  /* GR_COLOR_HIGHLIGHTTEXT       */
	GR_RGB( 64,  64,  64),  /* GR_COLOR_GRAYTEXT            */

	/* menu backgrounds/text*/
	GR_RGB(160, 160, 160),	/* GR_COLOR_MENU                */
	GR_RGB(  0,   0,   0),  /* GR_COLOR_MENUTEXT            */

	/* window border and interior line under caption*/
	GR_RGB(  0,   0,   0),  /* GR_COLOR_WINDOWFRAME         */
	GR_RGB(160, 160, 160)	/* GR_COLOR_WINDOWFRAMELT        */
};
#endif

/*
 * Enlarge/decrease the size of a rectangle
 */
void
nxInflateRect(GR_RECT *prc, GR_SIZE dx, GR_SIZE dy)
{
	prc->x -= dx;
	prc->y -= dy;
	prc->width += dx * 2;
	prc->height += dy * 2;
}

/*
 * nxDraw3dShadow
 * 	NOINDENT_BLACK	T=white, B=black
 * 	NOINDENT_GRAY	T=white, B=dkgray
 * 	INDENT_BLACK	T=black, B=white
 * 	INDENT_GRAY		T=dkgray, B=white
 *
 *	TTTTTTTTTTTTTT
 *	T             B
 *	T             B
 *	 BBBBBBBBBBBBBB
 */
void
nxDraw3dShadow(GR_DRAW_ID id,int x,int y,int w,int h,GR_COLOR crTop,
	GR_COLOR crBottom)
{
	GR_GC_ID	gc = GrNewGC();

	GrSetGCForeground(gc, crTop);
	/*MoveToEx( hDC, x, y+h-2, NULL);*/
	/*LineTo( hDC, x, y);*/				/* left side*/
	GrLine(id, gc, x, y+h-2, x, y);			/* left*/
	/*LineTo( hDC, x+w-1, y);*/			/* top side*/
	GrLine(id, gc, x, y, x+w-2, y);			/* top*/

	GrSetGCForeground(gc, crBottom);
	/*MoveToEx( hDC, x+w-1, y+1, NULL);*/
	/*LineTo( hDC, x+w-1, y+h-1);*/			/* right side*/
	GrLine(id, gc, x+w-1, y+1, x+w-1, y+h-2);	/* right*/
	/*LineTo( hDC, x, y+h-1);*/			/* bottom side*/
	GrLine(id, gc, x+w-1, y+h-1, x, y+h-1);		/* bottom*/

	GrDestroyGC(gc);
}

/*
 * nxDraw3dBox
 *
 *	TTTTTTTTTTTTTTB
 *	T             B
 *	T             B
 *	BBBBBBBBBBBBBBB
 */
void
nxDraw3dBox(GR_WINDOW_ID id,int x,int y,int w,int h,GR_COLOR crTop,
	GR_COLOR crBottom)
{
	GR_GC_ID	gc = GrNewGC();

	GrSetGCForeground(gc, crTop);
	/*MoveToEx( hDC, x, y+h-2, NULL);*/
	/*LineTo( hDC, x, y);*/				/* left side*/
	GrLine(id, gc, x, y+h-2, x, y+1);		/* left*/
	/*MoveToEx( hDC, x, y, NULL);*/
	/*LineTo( hDC, x+w-1, y);*/			/* top side*/
	GrLine(id, gc, x, y, x+w-2, y);			/* top*/

	GrSetGCForeground(gc, crBottom);
	GrLine(id, gc, x+w-1, y, x+w-1, y+h-2);		/* right*/
	/*MoveToEx( hDC, x+w-1, y, NULL);*/
	/*LineTo( hDC, x+w-1, y+h-1);*/			/* right side*/
	GrLine(id, gc, x+w-1, y+h-1, x, y+h-1);		/* bottom*/
	/*LineTo( hDC, x-1, y+h-1);*/			/* bottom side*/

	GrDestroyGC(gc);
}

/*
 * Draw 2 line deep 3d inset
 */
void
nxDraw3dInset(GR_DRAW_ID id,int x,int y,int w,int h)
{
	nxDraw3dBox(id, x, y, w, h,
		GrGetSysColor(GR_COLOR_BTNSHADOW),
		GrGetSysColor(GR_COLOR_BTNHIGHLIGHT));
	++x; ++y; w -= 2; h -= 2;
	nxDraw3dBox(id, x, y, w, h,
		GrGetSysColor(GR_COLOR_3DFRAME),
		GrGetSysColor(GR_COLOR_3DLIGHT));
}

/*
 * Draw 2 line deep 3d outset
 */
void
nxDraw3dOutset(GR_DRAW_ID id,int x,int y,int w,int h)
{
	nxDraw3dBox(id, x, y, w, h,
		GrGetSysColor(GR_COLOR_3DLIGHT),
		GrGetSysColor(GR_COLOR_3DFRAME));
	++x; ++y; w -= 2; h -= 2;
	nxDraw3dBox(id, x, y, w, h,
		GrGetSysColor(GR_COLOR_BTNHIGHLIGHT),
		GrGetSysColor(GR_COLOR_BTNSHADOW));
}

/*
 * Draw 1 line pushed down rectangle
 */
void
nxDraw3dPushDown(GR_DRAW_ID id, int x, int y, int w, int h)
{
	nxDraw3dBox(id, x, y, w, h, GrGetSysColor(GR_COLOR_BTNSHADOW),
		GrGetSysColor(GR_COLOR_BTNSHADOW));
}

/*
 * Draw either 3d up or down depending on state
 */
void
nxDraw3dUpDownState(GR_DRAW_ID id, int x, int y, int w, int h, GR_BOOL fDown)
{
	if (fDown)
		nxDraw3dPushDown(id, x, y, w, h);
	else nxDraw3dOutset(id, x, y, w, h);
}

#if 0
void
nxDraw3dUpFrame(GR_DRAW_ID id, int l, int t, int r, int b)
{
	RECT	rc;
	HBRUSH	hbr;

	SetRect(&rc, l, t, r, b);
	nxDraw3dBox(hDC, rc.left, rc.top,
		rc.right-rc.left, rc.bottom-rc.top,
		GrGetSysColor(GR_COLOR_3DLIGHT),
		GrGetSysColor(GR_COLOR_3DFRAME));
	nxInflateRect(&rc, -1, -1);
	nxDraw3dBox(hDC, rc.left, rc.top,
		rc.right-rc.left, rc.bottom-rc.top,
		GrGetSysColor(GR_COLOR_BTNHIGHLIGHT),
		GrGetSysColor(GR_COLOR_BTNSHADOW));
	nxInflateRect(&rc, -1, -1);

	hbr = CreateSolidBrush(GrGetSysColor(GR_COLOR_APPWINDOW));
	FillRect(hDC, &rc, hbr);
	DeleteObject(hbr);
}
#endif
