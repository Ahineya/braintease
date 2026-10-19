#!/usr/bin/env python3
"""Checked workspace hooks around the pinned NanoWM implementation."""
from adapt import replace
from server_adapt import body


def adapt_workspace(dest):
    path = dest / 'nanox/client.c'
    path.write_text(path.read_text() + '\n#include "workspace-client.inc"\n')
    replace(dest, 'nanox/srvnet.c', ' current_fd=fd;',
            ' current_fd=fd;\n if(r->reqType==252){nxr_workspace_dispatch(fd,packet);return 0;}')
    # Focus checks apply to pointer, keyboard and explicit client activation.
    replace(dest, 'nanox/srvutil.c', '\tGR_WINDOW\t*oldfocus;',
            '\tGR_WINDOW\t*oldfocus;\n\tif (!nxr_workspace_focusable(wp)) return;')
    replace(dest, 'demos/nanox/nanowm.c', '#include "nanowm.h"',
            '#include "nanowm.h"\n#include "fde_workspace.h"')
    replace(dest, 'demos/nanox/nanowm.c', '\twm_init();',
            '\tif(fde_workspace_manage()!=FDE_OK){GrError("nanowm: window manager already running or unsupported\\n");GrClose();return 1;}\n'
            '\twm_init();\n'
            '    {GR_WINDOW_ID parent,*children;GR_COUNT count,i;GR_WINDOW_INFO info;\n'
            '     GrQueryTree(GR_ROOT_WINDOW_ID,&parent,&children,&count);\n'
            '     for(i=0;i<count;i++){GrGetWindowInfo(children[i],&info);if(info.mapped)wm_new_client_window(children[i]);}\n'
            '     free(children);}\n')
    replace(dest, 'nanox/wmclients.c', '#include "nanowm.h"',
            '#include "nanowm.h"\n#include "fde_workspace.h"')
    replace(dest, 'nanox/wmclients.c', '\t/* get client window information*/',
            '\tif(wm_find_window(wid))return 1;\n\t/* get client window information*/')
    replace(dest, 'nanox/wmclients.c', '\tGrReparentWindow(wid, pid, xoffset, yoffset);',
            '\tGrReparentWindow(wid, pid, xoffset, yoffset);\n'
            '\tif(fde_workspace_frame(wid,pid)!=FDE_OK)GrError("nanowm: frame registration failed\\n");')
    replace(dest, 'nanox/wmclients.c', '\tGrGetWindowInfo(pwin->clientid, &winfo);',
            '\tGrGetWindowInfo(pwin->clientid, &winfo);\n\tif(!winfo.wid)return; /* queued resize after client exit */')
    # Thin artwork is independent of the interaction border. Reserve a visible
    # five-pixel frame so pointer hits never consume application content.
    replace(dest, 'include/nanowm.h', '#define CXBORDER\t1', '#define CXBORDER\t5')
    replace(dest, 'include/nanowm.h', '#define CYBORDER\t1', '#define CYBORDER\t5')
    replace(dest, 'include/nanowm.h', '#define CYCAPTION\t29', '#define CYCAPTION\t20')
    replace(dest, 'nanox/wmutil.c', '\tw->sizing = GR_FALSE;', '\tw->close = GR_FALSE;\n\tw->sizing = GR_FALSE;')
    # Local interaction/painting code shares the upstream window list and event
    # routing. It avoids XOR full-screen resize outlines and clamps geometry.
    for name in ('wm_container_buttondown', 'wm_container_buttonup', 'wm_container_mousemoved',
                 'wm_container_mouse_enter', 'wm_container_mouse_exit', 'wm_redraw_ncarea'):
        args = {'wm_container_buttondown':'event', 'wm_container_buttonup':'event',
                'wm_container_mousemoved':'event', 'wm_container_mouse_enter':'event',
                'wm_container_mouse_exit':'event'}.get(name)
        body(dest, 'nanox/wmaction.c', name, '    fde_' + name + '(window' + (', '+args if args else '') + ');')
    path = dest / 'nanox/wmaction.c'
    text = path.read_text().replace('static void set_resize_cursor(int wid);',
            'static void set_resize_cursor(int wid);\n#include "wm-workspace.inc"')
    path.write_text(text)
    # Cancellation/unmap destroys drag state in both move and resize modes.
    path = dest / 'nanox/wmclients.c'
    text = path.read_text().replace('if(pwin->active) {', 'if(pwin->active || pwin->sizing) {')
    text = text.replace('pwin->active = GR_FALSE;', 'pwin->active = pwin->sizing = GR_FALSE;')
    path.write_text(text)
