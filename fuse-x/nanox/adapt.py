#!/usr/bin/env python3
"""Apply rootless hooks only to the native generated Nano-X server sources."""
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[2]
NX = ROOT / 'ports/nanox'


def function(text, name, change):
    pattern = r'\b' + re.escape(name) + r'\s*\([^;{}]*\)\s*\{'
    match = re.search(pattern, text)
    if not match:
        raise RuntimeError(f'missing native hook function {name}')
    begin = text.index('{', match.start())
    masked = re.sub(r'/\*.*?\*/|//[^\n]*|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'',
                    lambda m:' ' * len(m.group()), text, flags=re.S)
    depth, end = 1, begin + 1
    while depth:
        depth += (masked[end] == '{') - (masked[end] == '}')
        end += 1
    body = text[begin+1:end-1]
    return text[:begin+1] + change(body) + text[end-1:]


def replace(text, old, new, count=1):
    if text.count(old) != count:
        raise RuntimeError(f'native hook contract changed ({text.count(old)} != {count}): {old!r}')
    return text.replace(old,new)


def adapt(dest):
    path=dest/'nanox/client.c';text=path.read_text()
    text='#ifdef NXR_NATIVE_GUI\n#include "session.h"\n#endif\n'+text
    text=function(text,'GrOpen',lambda s:replace(s,'\n\treturn nxSocket;\n',
        '#ifdef NXR_NATIVE_GUI\n    nxh_detach_terminal();\n#endif\n\treturn nxSocket;\n'))
    path.write_text(text)
    text=(NX/'ripple/nxterm.c').read_text()
    text=replace(text,'signal(SIGHUP,caught);','signal(SIGHUP,SIG_IGN);')
    text=replace(text,'putenv("TERM=ansi");execvp(',
        'signal(SIGHUP,SIG_DFL);putenv("TERM=ansi");execvp(')
    (dest/'native-nxterm.c').write_text(text)
    for name in ('srvfunc', 'srvutil', 'srvclip'):
        path=dest / f'nanox/{name}.c'
        text=path.read_text()
        text='#include "native.h"\n'+text
        if name=='srvfunc':
            text=function(text,'NewWindow',lambda s:replace(replace(s,'wp->psd = rootwp->psd;', 'wp->psd = pwp->psd;'),
                '\tpwp->children = wp;', '\tif(!nxh_new(wp)){free(wp);GsError(GR_ERROR_MALLOC_FAILED,0);return NULL;}\n\tpwp->children = wp;'))
            text=function(text,'GrResizeWindow',lambda s:replace(s,'\t/* possibly reallocate buffered window',
                '\tif(!nxh_resize(wp,width,height)){GsError(GR_ERROR_MALLOC_FAILED,wid);SERVER_UNLOCK();return;}\n\tclipwp=NULL;\n\t/* possibly reallocate buffered window'))
            text=function(text,'GrMoveWindow',lambda s:"""

    GR_WINDOW *wp;SERVER_LOCK();wp=GsFindWindow(wid);
    if(!wp||wp==rootwp||wp->parent==rootwp){SERVER_UNLOCK();return;}
    x+=wp->parent->x;y+=wp->parent->y;
    GsUnrealizeWindow(wp,GR_TRUE);OffsetWindow(wp,x-wp->x,y-wp->y);clipwp=NULL;
    GsRealizeWindow(wp,GR_FALSE);DeliverUpdateMoveEventAndChildren(wp);SERVER_UNLOCK();
""")
            text=function(text,'GrRaiseWindow',lambda s:replace(s,'\tprevwp = wp->parent->children;',
                '\tif(wp->parent==rootwp)nxh_activate(wp);\n\tprevwp = wp->parent->children;'))
            text=function(text,'GrSetFocus',lambda s:replace(s,'\tGsSetFocus(wp);','\tGsSetFocus(wp);nxh_activate(wp);'))
            text=function(text,'GrReparentWindow',lambda s:replace(replace(s,'\tx += pwp->x;',
                '\tif(!nxh_reparent(wp,pwp)){GsError(GR_ERROR_MALLOC_FAILED,wid);SERVER_UNLOCK();return;}\n\tx += pwp->x;'),
                '\tGsRealizeWindow(wp, GR_TRUE);', '\tnxh_reparented(wp);\n\tGsRealizeWindow(wp, GR_TRUE);'))
            text=function(text,'GrSetWMProperties',lambda s:replace(s,'\toldprops = wp->props;',
                '\tif(wp->parent==rootwp)props->bordersize=0;\n\toldprops = wp->props;'))
        if name=='srvutil':
            text=function(text,'GsDestroyWindow',lambda s:replace(s,'free(wp);','nxh_destroy(wp);free(wp);'))
            text=function(text,'GsFindVisibleWindow',lambda s:'\n return nxh_mouse_window(x,y);\n')
        if name=='srvclip':
            # srvclip.c textually includes srvclip2.c below; generated separately.
            pass
        path.write_text(text)
    path=dest/'nanox/srvclip2.c'
    text=path.read_text()
    text=replace(text,'while (pwp != rootwp) {','while (pwp->parent && pwp->parent != rootwp) {')
    text=replace(text,'while (pwp != NULL) {','while (pwp && pwp->parent != rootwp) {')
    # The virtual root has no presentation surface; never draw desktop pixels.
    text=replace(text,'\tif (!wp->realized || !wp->output)',
        '\tif(wp==rootwp){clipwp=wp;GdSetClipRegion(wp->psd,GdAllocRegion());return;}\n\tif (!wp->realized || !wp->output)')
    text=replace(text,'vis = GdAllocRectRegion(x, y, x+width, y+height);',
        'vis = GdAllocRectRegion(x, y, x+width, y+height);\n'
        '    for(pwp=wp->parent;pwp && pwp!=rootwp;pwp=pwp->parent)if(pwp->clipregion){\n'
        '      GdOffsetRegion(pwp->clipregion,pwp->x,pwp->y);GdIntersectRegion(vis,vis,pwp->clipregion);\n'
        '      GdOffsetRegion(pwp->clipregion,-pwp->x,-pwp->y);}')
    path.write_text(text)

    text=(NX/'ripple/screen.c').read_text()
    text='#include "native.h"\n'+text
    text=function(text,'damage',lambda s:'\n nxh_damage(psd,x,y,width,height);\n')
    for name in ('drawpixel','horizontal','vertical','fill','frameblit','monochrome'):
        text=function(text,name,lambda s:'\n if(psd==&scrdev)return;\n'+s)
    text=function(text,'readpixel',lambda s:'\n if(psd==&scrdev)return 0;\n'+s)
    text=function(text,'GdBlit',lambda s:'\n if(src==&scrdev||dst==&scrdev)return;\n'+s)
    text=function(text,'screen_open',lambda s:'''\n
    if(fd>=0)return NULL;
    fd=open("/dev/gfx",O_RDWR|O_CLOEXEC);if(fd<0)return NULL;
    allocation=calloc(3,sizeof(*allocation));if(!allocation){close(fd);fd=-1;return NULL;}
    psd->flags=PSF_SCREEN;psd->xres=psd->xvirtres=640;psd->yres=psd->yvirtres=480;
    psd->planes=1;psd->bpp=16;psd->data_format=MWIF_RGB565;psd->pitch=640;
    psd->size=1;psd->addr=(unsigned char *)(allocation+1);psd->ncolors=65536UL;
    psd->pixtype=MWPF_TRUECOLOR565;psd->builtin_fonts=gen_fonts;
    psd->Close=screen_close;psd->GetScreenInfo=screen_info;psd->Update=damage;
    psd->DrawPixel=drawpixel;psd->ReadPixel=readpixel;psd->DrawHorzLine=horizontal;
    psd->DrawVertLine=vertical;psd->FillRect=fill;psd->FrameBlit=frameblit;
    psd->BlitCopyMaskMonoWordMSB=monochrome;return psd;
''')
    text=function(text,'screen_close',lambda s:'''\n
    nxh_finish();if(fd>=0)close(fd);fd=-1;free(allocation);allocation=NULL;psd->addr=NULL;
    if(clipregion){GdDestroyRegion(clipregion);clipregion=NULL;}
''')
    text=function(text,'nxr_present',lambda s:'\n return nxh_present();\n')
    text=function(text,'nxr_event',lambda s:'\n return nxh_event(e);\n')
    text=replace(text,'width>640||height>480','width>1024||height>768')
    (dest/'native-screen.c').write_text(text)

    text=(NX/'ripple/server.c').read_text()
    text='#include "native.h"\n'+text
    text=replace(text,'while(nxr_event(&e)==0&&e.type){','while(nxr_event(&e)==0&&e.type){\n        if(!nxh_input(&e))continue;')
    text+='''\nvoid nxh_release_keys(void) {
 unsigned i;MWKEY k;
 for(i=0;i<256;i++)if(held[i>>4]&(1U<<(i&15))){k=key(i);if(k)GsDeliverKeyboardEvent(0,GR_EVENT_TYPE_KEY_UP,k,0,i);}
 memset(held,0,sizeof(held));modifiers=0;repeat_key=0;GsHandleMouseStatus(cursorx,cursory,0);
}\n'''
    # Native windows may grow beyond the virtual root's default screen size.
    text=text.replace('<=640','<=1024').replace('<=480','<=768')
    (dest/'native-server.c').write_text(text)
    # The launcher is an ordinary titled native window, not a desktop panel.
    path=dest/'demos/nanox/nxlaunch.c';text=path.read_text()
    text=replace(text,'GrSetWMProperties(state->main_window, &props);',
        'props.flags |= GR_WM_FLAGS_TITLE;props.title="Applications";props.props |= GR_WM_PROPS_NORESIZE;\n\tGrSetWMProperties(state->main_window, &props);')
    path.write_text(text)


if __name__=='__main__':
    adapt(Path(sys.argv[1]))
