#!/usr/bin/env python3
"""Checked adaptations of the pinned server, client library and NanoWM for C16."""
from pathlib import Path
import re
import shutil
from adapt import replace
HERE = Path(__file__).resolve().parent
SERVER = 'srvmain srvfunc srvutil srvevent srvclip srvnet'.split()
CLIENT = 'client nxproto nxdraw nxpaintnc nxutil'.split()
WM = 'wmaction wmclients wmevents wmutil'.split()


def body(root, name, function, replacement):
    path = root / name
    text = path.read_text()
    match = re.search(r'\n(?:static )?(?:void|int|long|GR_BOOL|MWBOOL)\s+\n?' + function + r'\s*\([^;]*?\)\s*\{', text)
    if not match:
        raise RuntimeError(f'{name}: missing function {function}')
    start = text.index('{', match.start())
    # Strip comments/strings while retaining positions before balancing braces.
    tokens = re.compile(r'/\*.*?\*/|//[^\n]*|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'', re.S)
    masked = tokens.sub(lambda m: ' ' * len(m.group()), text)
    depth, end = 1, start + 1
    while depth:
        if masked[end] == '{': depth += 1
        if masked[end] == '}': depth -= 1
        end += 1
    path.write_text(text[:start] + '{\n' + replacement + '\n}' + text[end:])


def adapt_server(dest):
    src = HERE / 'upstream/src'
    names = [*(f'nanox/{s}.c' for s in set(SERVER + CLIENT + WM)),
             'nanox/serv.h', 'nanox/nxproto.h', 'nanox/srvclip1.c', 'nanox/srvclip2.c', 'drivers/genmem.h', 'engine/devmouse.c',
             'engine/devkbd.c', 'engine/devtimer.c',
             'demos/nanox/nanowm.c', 'demos/nanox/nxlaunch.c', 'demos/nanox/nxlaunch.h']
    for name in names:
        (dest / name).parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(src / name, dest / name)
    # A private, same-C16-ABI protocol, deliberately not the upstream octet wire ABI.
    # No four-octet manual padding assumptions: RCC structs are naturally cell aligned.
    replace(dest, 'nanox/nxproto.h', '#define ALIGNSZ\t\t4', '#define ALIGNSZ\t\t1')
    replace(dest, 'nanox/nxproto.h', '#define MAXREQUESTSZ\t30000', '#define MAXREQUESTSZ\t4096')
    replace(dest, 'nanox/nxproto.h', '(((req)->hilength << 16) | (req)->length)',
            '(((unsigned long)(req)->hilength << 16) | (req)->length)')
    replace(dest, 'nanox/nxproto.h', 'UINT32\tpid;\n} nxOpenReq;',
            'UINT32\tpid;\n    UINT32 c16_magic; /* versioned local C16 ABI handshake */\n} nxOpenReq;')
    replace(dest, 'nanox/client.c', 'req.pid = getpid();', 'req.pid = getpid();\n    req.c16_magic = 0x4e583102UL;')
    # Advertise only the private C16 socket name, including a synchronous handshake.
    replace(dest, 'nanox/client.c', 'EPRINTF("nxclient: lost connection to Nano-X server\\n");',
            'if(getenv("NANOX_SESSION"))exit(0);\n\t\t\t\tEPRINTF("nxclient: lost connection to Nano-X server\\n");')
    replace(dest, 'nanox/client.c', '#define GR_CLOSE_FIX\t1', '#define GR_CLOSE_FIX\t0')
    replace(dest, 'nanox/client.c', 'nxWriteSocket((char *)&req,sizeof(req));\n\treturn nxSocket;',
            'nxWriteSocket((char *)&req,sizeof(req));\n    { unsigned long ack=0; unsigned received=0; int n;\n      while(received<sizeof(ack)){n=read(nxSocket,(char *)&ack+received,sizeof(ack)-received);\n        if(n<0&&errno==EINTR)continue;if(n<=0)break;received+=n;}\n      if(received!=sizeof(ack)||ack!=req.c16_magic){close(nxSocket);nxSocket=-1;return -1;} }\n\treturn nxSocket;')
    # Public event structures use target pointers/widths on both ends. Optional
    # image/shm APIs stay disabled; a checked dispatcher validates every exposed frame.
    p = dest / 'include/nano-X.h'
    s = p.read_text(); s = s.replace('"/tmp/.nano-X"', '"/tmp/.nano-X16"'); p.write_text(s)
    replace(dest, 'nanox/srvmain.c', '#include "osdep.h"', '#include "osdep.h"\n#include "server.h"\n#include <sys/select.h>')
    body(dest, 'nanox/srvmain.c', 'GsSelect', '    nxr_select(timeout);')
    replace(dest, 'nanox/srvmain.c', 'client->id = i;', 'memset(client, 0, sizeof(*client));\n\tclient->id = i;')
    replace(dest, 'nanox/srvnet.c', '#include "nxproto.h"', '#include "nxproto.h"\n#include "server.h"')
    replace(dest, 'nanox/srvfunc.c', 'regionp->rgn = GdAllocBitmapRegion(bitmap, width, height);',
            'regionp->rgn = GdAllocBitmapRegion(bitmap, width, height);\n\tif(!regionp->rgn) { free(regionp); SERVER_UNLOCK(); return 0; }')
    # Line dashes use the core drawing engine, even with optional shapes off.
    replace(dest, 'nanox/srvutil.c', '#if MW_FEATURE_SHAPES\n\t\tGdSetDash(&mask, &count);',
            '\t\tGdSetDash(&mask, &count);\n#if MW_FEATURE_SHAPES')
    replace(dest, 'nanox/srvfunc.c', '(1 << dcount)', '(1UL << dcount)')
    # Match the sibling clipping path for shaped children too. Upstream first
    # subtracted the entire rectangle, then tested the parent's region and
    # could dereference a NULL child region.
    replace(dest, 'nanox/srvclip2.c', '''\t\t\tGdSetRectRegion(r, minx, miny, maxx, maxy);
\t\t\tGdSubtractRegion(vis, vis, r);
\t\t\t/* FIXME: shaped windows with borders won't work */
\t\t\tif (wp->clipregion) {
\t\t\t\t/* FIXME: can user set invalid clipregion here? */
\t\t\t\tGdOffsetRegion(sibwp->clipregion, sibwp->x, sibwp->y);
\t\t\t\tGdSubtractRegion(vis, vis, sibwp->clipregion);
\t\t\t\tGdOffsetRegion(sibwp->clipregion, -sibwp->x, -sibwp->y);
\t\t\t}''', '''            GdSetRectRegion(r, minx, miny, maxx, maxy);
            if (sibwp->clipregion) {
                GdOffsetRegion(sibwp->clipregion, sibwp->x, sibwp->y);
                GdIntersectRegion(r, r, sibwp->clipregion);
                GdOffsetRegion(sibwp->clipregion, -sibwp->x, -sibwp->y);
            }
            GdSubtractRegion(vis, vis, r);''')
    for name in ['GrEllipseWrapper', 'GrFillEllipseWrapper']:
        body(dest, 'nanox/srvnet.c', name, '    GrNotImplementedWrapper(r); /* shapes disabled in this profile */')
    body(dest, 'nanox/srvnet.c', 'GsWrite', '    return nxr_write(fd, buf, c);')
    body(dest, 'nanox/srvnet.c', 'GsHandleClient', '    nxr_receive(fd);')
    replace(dest, 'nanox/srvnet.c', 'curclient->processid = req->pid;',
            'curclient->processid = req->pid;\n    GsWrite(current_fd, &req->c16_magic, sizeof(req->c16_magic));')
    replace(dest, 'nanox/srvnet.c', 'GsAcceptClientFd(i);',
            'if (nxr_accept(i) < 0) { close(i); return; }\n\tGsAcceptClientFd(i); if(!GsFindClient(i))nxr_forget(i);')
    replace(dest, 'nanox/srvnet.c', 'GsDropClient(int fd)\n{', 'GsDropClient(int fd)\n{\n    nxr_forget(fd);')
    replace(dest, 'nanox/srvevent.c', '#include "osdep.h"', '#include "osdep.h"\n#include "server.h"')
    replace(dest, 'nanox/srvevent.c', 'EVENT_UNLOCK(&eventMutex);\n\telp = eventfree;',
        'EVENT_UNLOCK(&eventMutex);\n    { unsigned count=0; GR_EVENT_LIST *queued=client->eventhead; static GR_EVENT discarded;\n      while(queued){++count;queued=queued->next;}\n      if(count>=64){nxr_bad(client->id);memset(&discarded,0,sizeof(discarded));return &discarded;} }\n\telp = eventfree;')
    replace(dest, 'nanox/srvevent.c', 'elp->event.type = GR_EVENT_TYPE_NONE;', 'memset(&elp->event, 0, sizeof(elp->event));')
    # Export the existing upstream dispatcher; platform transport supplies complete,
    # length-validated frames only. Unsupported protocol calls are refused.
    with (dest / 'nanox/srvnet.c').open('a') as out:
        out.write('\nint nxr_dispatch(int fd, void *packet) {\n nxReq *r=packet;\n curclient=GsFindClient(fd); if(!curclient)return -1;\n current_fd=fd; curfunc=(char *)GrFunctions[r->reqType].name;\n GrFunctions[r->reqType].func(r); return 0;\n}\n')
    # Keep upstream timer list/callbacks; sample monotonic time and normalize C16
    # unsigned timeval arithmetic explicitly.
    replace(dest, 'engine/devtimer.c', '#include "device.h"', '#include "device.h"\n#include "server.h"')
    p = dest / 'engine/devtimer.c';s=p.read_text().replace('gettimeofday(&current_time, NULL)', 'nxr_time(&current_time)').replace('gettimeofday (&current_time, NULL)', 'nxr_time(&current_time)');p.write_text(s)
    body(dest, 'engine/devtimer.c', 'time_to_expiry',
         '    long seconds=(long)(t->tv_sec-current_time.tv_sec);\n    long micros=(long)t->tv_usec-(long)current_time.tv_usec;\n    if(seconds>2147482L)return 2147483647L;\n    if(seconds< -2147482L)return -2147483647L;\n    return seconds*1000L + (micros>=0?(micros+999L)/1000L:micros/1000L);')
    p=dest/'engine/devtimer.c';s=p.read_text().replace('tv->tv_usec > 1000000','tv->tv_usec >= 1000000');p.write_text(s)
    replace(dest, 'demos/nanox/nxlaunch.c', '#include <time.h>', '#include "mwconfig.h"\n#include <time.h>')
    replace(dest, 'demos/nanox/nanowm.c', '#include <stdio.h>', '#include <stdio.h>\n#include <unistd.h>')
    replace(dest, 'demos/nanox/nanowm.c', 'wm_init();', 'wm_init();\n    if(argc==2){GR_SCREEN_INFO ready;int fd=atoi(argv[1]);GrGetScreenInfo(&ready);write(fd,"R",1);close(fd);}')
    body(dest, 'nanox/srvnet.c', 'GsOpenSocket', '    return nxr_open_socket();')
    # Keep keyboard focus on the actual client when frame interaction changes
    # geometry: upstream unrealize/realize can temporarily reset it to root.
    replace(dest, 'nanox/wmaction.c', 'GrSetFocus(window->wid);', 'GrSetFocus(window->clientid);')
    replace(dest, 'nanox/wmaction.c', 'window->sizing = GR_FALSE;', 'window->sizing = GR_FALSE;\n      GrSetFocus(window->clientid);')
    # Text-only launcher profile: don't send image requests for empty icons.
    replace(dest, 'demos/nanox/nxlaunch.c', 'GrDrawImageToFit(item->wid, state->gc,', 'if(MW_FEATURE_IMAGES && item->iconid) GrDrawImageToFit(item->wid, state->gc,')
    replace(dest, 'demos/nanox/nxlaunch.c', '#define CONFIG\t"bin/nxlaunch.cnf"', '#define CONFIG\t"/etc/nxlaunch.cnf"')
    replace(dest, 'demos/nanox/nxlaunch.c', 'if(strcmp("-", icon)) {', 'if(!MW_FEATURE_IMAGES && strcmp("-", icon)) { GrError("nxlaunch: this profile supports text buttons only\\n"); exit(2); }\n\tif(MW_FEATURE_IMAGES && strcmp("-", icon)) {')
    replace(dest, 'demos/nanox/nxlaunch.c', 'if(state->window_background_image) {', 'if(MW_FEATURE_IMAGES && state->window_background_image) {')
    # Fix a pre-existing precedence bug in the launcher child reaper.
    replace(dest,'demos/nanox/nxlaunch.c','while((pid = waitpid(-1, NULL, WNOHANG) > 0))','while((pid = waitpid(-1, NULL, WNOHANG)) > 0)')

    from paint_adapt import adapt_paint
    adapt_paint(dest)
    from event_adapt import adapt_events
    adapt_events(dest)
    from image_adapt import adapt_images
    adapt_images(dest)
    from workspace_adapt import adapt_workspace
    adapt_workspace(dest)
    from clipboard_adapt import adapt_clipboard
    adapt_clipboard(dest)

    # The private protocol exposes a fixed server color table, with no mutation
    # request. Cache successful queries per connection.
    replace(dest,'nanox/client.c','\tnxGetSysColorReq *req;\n\tGR_COLOR color;',
            '\tstatic GR_COLOR colors[22];static unsigned char valid[22];\n\tnxGetSysColorReq *req;\n\tGR_COLOR color;\n\tif(index>=0&&index<22&&valid[index])return colors[index];')
    replace(dest,'nanox/client.c','\t\tcolor = 0;\n\tUNLOCK(&nxGlobalLock);\n\treturn color;',
            '\t\tcolor = 0;\n\telse if(index>=0&&index<22){colors[index]=color;valid[index]=1;}\n\tUNLOCK(&nxGlobalLock);\n\treturn color;')
