"""Retain bounded, counted wheel gestures instead of disconnecting readers."""
from adapt import replace


def adapt_events(dest):
    replace(dest, 'nanox/serv.h', '\tGR_EVENT\tevent;\t\t/* event */',
            '\tGR_EVENT\tevent;\t\t/* event */\n\tunsigned c16_repeats; /* server-private wheel run */')
    replace(dest, 'nanox/srvevent.c', '#include "server.h"',
            '#include "server.h"\n#include "event-queue.inc"')
    replace(dest, 'nanox/srvevent.c', 'memset(&elp->event, 0, sizeof(elp->event));',
            'memset(&elp->event, 0, sizeof(elp->event));elp->c16_repeats=1;')
    replace(dest, 'nanox/srvevent.c', '\tGR_EVENT_MASK\ttempmask;\t/* to get around compiler bug */\n\n\teventmask = GR_EVENTMASK(type);',
            '\tGR_EVENT_MASK\ttempmask;\t/* to get around compiler bug */\n\n'
            '    /* Wheels are pulses, not held buttons. No synthetic release. */\n'
            '    if(type==GR_EVENT_TYPE_BUTTON_UP){\n'
            '        changebuttons&=~(GR_BUTTON_SCROLLUP|GR_BUTTON_SCROLLDN);\n'
            '        if(!changebuttons)return;\n'
            '    }\n\teventmask = GR_EVENTMASK(type);')
    replace(dest, 'nanox/srvevent.c', '(grabbuttonwp == NULL)) {',
            '(grabbuttonwp == NULL) &&\n'
            '                ((changebuttons&~(GR_BUTTON_SCROLLUP|GR_BUTTON_SCROLLDN)) ||\n'
            '                 !(buttons&(GR_BUTTON_SCROLLUP|GR_BUTTON_SCROLLDN)))) {')
    replace(dest, 'nanox/srvevent.c', 'ep = (GR_EVENT_BUTTON *) GsAllocEvent(client);',
            'ep = (GR_EVENT_BUTTON *) nxr_button_event(client,wp->id,subwid,buttons,changebuttons,modifiers);')
    replace(dest, 'nanox/srvfunc.c', '\t/* Remove first event from queue*/',
            '    /* Replay a counted wheel run without allocating more nodes. */\n'
            '    if(elp->c16_repeats>1){--elp->c16_repeats;return;}\n'
            '\t/* Remove first event from queue*/')
