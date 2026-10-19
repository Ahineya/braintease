#!/usr/bin/env python3
"""C16 buffered repaints: retain exposure-driven apps and commit child widgets together."""
from adapt import replace
from server_adapt import body


def adapt_paint(dest):
    replace(dest,'nanox/srvfunc.c','#include "serv.h"','#include "serv.h"\n#include "server.h"')
    p=dest/'include/nano-X.h'
    p.write_text(p.read_text()+'\n/* Private C16 exposure-preserving backing buffers. */\n#define GR_WM_PROPS_C16_BATCHED 0x00002000UL\n')
    p=dest/'nanox/nxproto.h'
    p.write_text(p.read_text()+'\n#define NXR_COMMIT 254\n')
    replace(dest,'nanox/client.c','\tnxFlushReq(0L,1);\n  ',
            '\tnxAllocReq(NXR_COMMIT,sizeof(nxReq),0);\n\tnxFlushReq(0L,1);\n  ')
    # Each buffer has one accumulated damage rectangle. No full-window copy
    # is required to publish a changed character or a moving game piece.
    replace(dest,'nanox/serv.h','\tGR_PIXMAP\t*buffer;\t/* window buffer pixmap*/',
            '\tGR_PIXMAP\t*buffer;\t/* window buffer pixmap*/\n\tint c16_dirty,c16_left,c16_top,c16_right,c16_bottom;')
    replace(dest,'nanox/srvfunc.c','\twp->buffer = NULL;',
            '\twp->buffer = NULL;\n\twp->c16_dirty=0;')
    replace(dest,'nanox/srvfunc.c','\t\tsrcpsd = swp->psd;\n\t\tsrcx += swp->x;\n\t\tsrcy += swp->y;',
            '\t\tif(swp->buffer){srcpsd=swp->buffer->psd;exposure=GR_FALSE;}\n\t\telse{srcpsd=swp->psd;srcx+=swp->x;srcy+=swp->y;}')
    replace(dest,'nanox/srvfunc.c','\tif (gcp)\n\t\texposure = gcp->exposure;',
            '\tif (gcp)\n\t\texposure = gcp->exposure && !(swp && swp->buffer);')
    replace(dest,'nanox/srvfunc.c','\tid = wp? wp->id: 0;',
            '\tif(wp&&!nxr_buffer_window(wp)){GrDestroyWindow(wp->id);wp=NULL;}\n\tid = wp? wp->id: 0;')
    replace(dest,'nanox/srvfunc.c','\toldprops = wp->props;',
            '\toldprops = wp->props;\n\tif(wp!=rootwp&&wp->output&&(oldprops&GR_WM_PROPS_C16_BATCHED))\n'
            '\t\tprops->props|=GR_WM_PROPS_BUFFERED|GR_WM_PROPS_BUFFER_MWPF|GR_WM_PROPS_C16_BATCHED|(oldprops&GR_WM_PROPS_DRAWING_DONE);')
    replace(dest,'nanox/srvfunc.c','\t\tGsClearWindow(wp, x, y, width, height, exposeflag);',
            '\t\tif(!nxr_clear_buffer(wp,x,y,width,height,exposeflag))\n\t\t\tGsClearWindow(wp, x, y, width, height, exposeflag);')
    replace(dest,'nanox/srvutil.c','#include "serv.h"', '#include "serv.h"\n#include "server.h"')
    replace(dest,'nanox/srvutil.c','\t\t\tpp = wp->buffer;',
            '\t\t\tnxr_draw_target(wp);\n\t\t\tpp = wp->buffer;')
    replace(dest,'nanox/srvutil.c','\t\tGdFillRect(pp->psd, 0, 0, pp->width, pp->height);',
            '\t\tnxr_draw_target(wp);\n\t\tGdFillRect(pp->psd, 0, 0, pp->width, pp->height);')
    replace(dest,'nanox/srvutil.c','\tif (wp->props & GR_WM_PROPS_BUFFERED) {\n\n\t\t/* nothing to do',
            '\tif (wp->props & GR_WM_PROPS_BUFFERED) {\n'
            '\t\tif((wp->props&GR_WM_PROPS_C16_BATCHED)&&exposeflag==1)GsDeliverExposureEvent(wp,x,y,width,height);\n'
            '\t\t/* nothing to do')
    # Calling the public GrFlush is the commit boundary. Automatic request
    # buffer flushes needed for queries deliberately do not commit a repaint.
    p=dest/'nanox/srvnet.c';s=p.read_text().replace(' nxReq *r=packet;',
        ' nxReq *r=packet;\n nxr_draw_target(NULL);\n if(r->reqType==NXR_COMMIT){nxr_commit(fd);return 0;}')
    p.write_text(s)

    replace(dest,'nanox/srvevent.c','EPRINTF("nano-X: GsError ");',
            'EPRINTF("nano-X: GsError fd=%d ",curclient?curclient->id:-1);')

    # A window can be resized by another client (WM or a native control helper).
    # Its backing pixmap must retain the window owner's lifetime.
    replace(dest,'nanox/srvutil.c','\twp->buffer = GsFindPixmap(id);',
            '\twp->buffer = GsFindPixmap(id);\n\tif(wp->buffer)wp->buffer->owner=wp->owner;')
    replace(dest,'nanox/srvutil.c',
            '\t\twp->props &= ~(GR_WM_PROPS_BUFFERED | GR_WM_PROPS_DRAWING_DONE);',
            '\t\twp->props &= ~(GR_WM_PROPS_BUFFERED | GR_WM_PROPS_DRAWING_DONE | GR_WM_PROPS_BUFFER_MWPF | GR_WM_PROPS_C16_BATCHED);\n\t\twp->c16_dirty=0;')

    replace(dest,'nanox/nxproto.h','UINT32 c16_magic; /* versioned local C16 ABI handshake */',
            'UINT32 c16_magic; /* versioned local C16 ABI handshake */\n    UINT16 c16_direct; /* direct window decorations, not backing surfaces */')
    replace(dest,'nanox/client.c','req.c16_magic = 0x4e583102UL;',
            'req.c16_magic = 0x4e583102UL;req.c16_direct=getenv("NANOX_DIRECT")?1:0;')
    # The direct-mode flag is part of GrOpen's handshake, so set it before
    # connecting. Setting it at wm_init allocated a redundant full-size backing
    # pixmap for every decoration frame and exhausted the RVM server heap.
    replace(dest,'demos/nanox/nanowm.c','if(GrOpen() < 0)',
            'putenv("NANOX_DIRECT=1");\n\tif(GrOpen() < 0)')
    replace(dest,'nanox/wmclients.c','\twindow.wid = pid;',
            '\tif(!pid){GrError("nanowm: cannot allocate window frame\\n");return 0;}\n\twindow.wid = pid;')
    replace(dest,'nanox/wmclients.c','(style & ~GR_WM_PROPS_BUFFERED)',
            '(style & ~(GR_WM_PROPS_BUFFERED|GR_WM_PROPS_BUFFER_MWPF|GR_WM_PROPS_C16_BATCHED|GR_WM_PROPS_DRAWING_DONE))')
