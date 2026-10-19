/* SPDX-License-Identifier: GPL-2.0-or-later */
/* A trackpad gesture is a run of momentary wheel pulses. Keep its count in
 * server-private queue metadata, preserving the public event/protocol ABI.
 * Position updates may interleave, but buttons/keys/direction/window changes
 * end the run so their ordering is retained. */
static GR_EVENT *nxr_button_event(GR_CLIENT *client, GR_WINDOW_ID wid,
    GR_WINDOW_ID subwid, int buttons, int changed, int modifiers)
{
    GR_EVENT_LIST *node, *run=NULL;
    if((changed&(GR_BUTTON_SCROLLUP|GR_BUTTON_SCROLLDN)) &&
        !(changed&~(GR_BUTTON_SCROLLUP|GR_BUTTON_SCROLLDN))) {
        for(node=client->eventhead;node;node=node->next) {
            GR_EVENT_BUTTON *b=&node->event.button;
            if(node->event.type==GR_EVENT_TYPE_BUTTON_DOWN && b->wid==wid &&
                b->subwid==subwid && b->buttons==buttons &&
                b->changebuttons==changed && b->modifiers==modifiers)
                run=node;
            else if(node->event.type!=GR_EVENT_TYPE_MOUSE_POSITION)run=NULL;
        }
        if(run) {
            /* Bounded accumulated travel even if a client never reads. */
            if(run->c16_repeats<4096)++run->c16_repeats;
            return &run->event;
        }
    }
    return GsAllocEvent(client);
}
