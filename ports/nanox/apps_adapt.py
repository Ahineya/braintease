#!/usr/bin/env python3
"""Small checked adaptations of the pristine Nano-X demonstration clients."""
import shutil
from pathlib import Path
from adapt import replace

HERE = Path(__file__).resolve().parent


def adapt_apps(dest):
    from roach_adapt import adapt_roach
    adapt_roach(dest)
    for name in ('nxeyes.c', 'nxeyes.h', 'nxclock.c'):
        shutil.copyfile(HERE / 'upstream/src/demos/nanox' / name,
                        dest / 'demos/nanox' / name)
    replace(dest, 'demos/nanox/nxeyes.c', 'malloc(sizeof(nxeyes_state))',
            'calloc(1, sizeof(nxeyes_state))')
    replace(dest, 'demos/nanox/nxeyes.c', '#include <stdlib.h>',
            '#include <stdlib.h>\n#include <string.h>\n#include <unistd.h>')
    replace(dest, 'demos/nanox/nxeyes.c', 'props.flags = GR_WM_FLAGS_PROPS;',
            'memset(&props, 0, sizeof(props));\n\tprops.flags = GR_WM_FLAGS_PROPS | GR_WM_FLAGS_TITLE;\n\tprops.title = "Eyes";')
    replace(dest, 'demos/nanox/nxeyes.c', 'a = atan((double)dy / (double)dx);',
            '/* A vertical gaze must not divide by zero. */\n\t\ta = dx ? atan((double)dy / (double)dx) : 1.5707963267948966;')
    replace(dest, 'demos/nanox/nxeyes.c', 'start_blink_timer(state);\n\t\n\treturn state;',
            'start_blink_timer(state);\n\tif(!state->wid || !state->gc || !rid1 || !rid2 || !state->tid) return NULL;\n\tGrFlush();\n\tif(getenv("NANOX_TRACE")) fprintf(stderr,"NXEYES_READY %d\\n",getpid());\n\treturn state;')
    replace(dest, 'demos/nanox/nxeyes.c', 'draw_eyes(state, 1);\n}',
            'draw_eyes(state, 1);\n\tif(getenv("NANOX_TRACE")) fprintf(stderr,"NXEYES_BLINK %d\\n",state->eyes_closed);\n}')
    replace(dest, 'demos/nanox/nxeyes.c', 'GrClose();\n\tfree(state);',
            'if(getenv("NANOX_TRACE")) fprintf(stderr,"NXEYES_EXIT %d\\n",getpid());\n\tGrClose();\n\tfree(state);')
    # The clock is a fixed-size upstream client. Keep its 200-pixel integer
    # geometry, but don't let the WM advertise resizing that it cannot handle.
    replace(dest, 'demos/nanox/nxclock.c', 'GR_WM_PROPS_APPWINDOW, "Clock"',
            'GR_WM_PROPS_APPWINDOW | GR_WM_PROPS_NORESIZE | GR_WM_PROPS_NOAUTOMOVE, "Clock"')
    replace(dest, 'demos/nanox/nxclock.c', '-1, -1, CWIDTH, CHEIGHT',
            '400, 40, CWIDTH, CHEIGHT')
    replace(dest, 'demos/nanox/nxclock.c', 'GrMapWindow(w1);',
            'if(!w1 || !gc1 || !gc2 || !cid) { GrClose(); return 1; }\n\tGrMapWindow(w1);\n\tGrFlush();\n\tif(getenv("NANOX_TRACE")) fprintf(stderr,"NXCLOCK_READY %d\\n",getpid());')
    # Old hand angles start at -1; upstream indexed the trig table before it.
    replace(dest, 'demos/nanox/nxclock.c', 'if(angle < 91) {',
            'if(angle < 0 || angle > 360) return 0;\n\tif(angle < 91) {', 2)
    replace(dest, 'demos/nanox/nxclock.c', 'struct timeval tv;\n\tstruct timezone tz = {0};', '')
    replace(dest, 'demos/nanox/nxclock.c', 'gettimeofday(&tv, &tz);\n\tnow = tv.tv_sec - (60 * tz.tz_minuteswest);',
            'now = time(NULL);')
    replace(dest, 'demos/nanox/nxclock.c', 'draw_time(hour, minutes, sec, gc2);',
            'draw_time(hour, minutes, sec, gc2);\n\tif(getenv("NANOX_TRACE")) fprintf(stderr,"NXCLOCK_TICK %lu\\n",(unsigned long)now);')
    replace(dest, 'demos/nanox/nxclock.c', 'case GR_EVENT_TYPE_CLOSE_REQ:\n\t\t\t\tGrClose();',
            'case GR_EVENT_TYPE_CLOSE_REQ:\n\t\t\t\tif(getenv("NANOX_TRACE")) fprintf(stderr,"NXCLOCK_EXIT %d\\n",getpid());\n\t\t\t\tGrClose();')
