/* Exercise the real frontend's scrolling without requiring a physical mouse.
 * Only timer-delivered input is scripted; layout, painting and Nano-X are real. */
#include <nano-X.h>
#define main browser_main
#include "../frontend/nano.c"
#undef main
static void scripted_event(void *opaque)
{
    GR_EVENT storage, *event=&storage;
    (void)opaque;
    schedule(80,scripted_event,NULL);
    static unsigned stage;
    static unsigned passes;
    static int previous_y=-1;
    static uint64_t last;
    uint64_t now;
    nsu_getmonotonic_ms(&now);
    if (browser_window_has_content(window.bw) && now - last >= 80) {
        memset(event, 0, sizeof(*event));
        last = now;
        if (getenv("NETSURF_SCROLL_WHEEL") && passes < 2) {
            if (previous_y==window.sy && stage && loaded) {
                event->type=GR_EVENT_TYPE_KEY_DOWN;
                event->keystroke.ch=MWKEY_HOME;
                previous_y=-1;++passes;
            } else {
                previous_y=window.sy;
                event->type=GR_EVENT_TYPE_BUTTON_DOWN;
                event->button.x=150;event->button.y=200;
                event->button.changebuttons=GR_BUTTON_SCROLLDN;
            }
            fprintf(stderr,"NETSURF_SCROLL_CHECK %u previous_y=%d loading=%u\n",stage++,window.sy,loaded?0:1);
        } else if (!getenv("NETSURF_SCROLL_WHEEL") && stage < 13) {
            event->type = GR_EVENT_TYPE_KEY_DOWN;
            event->keystroke.ch = stage % 2 ? MWKEY_HOME : MWKEY_END;
            fprintf(stderr, "NETSURF_SCROLL_CHECK %u previous_y=%d loading=%u\n", stage, window.sy, loaded?0:1);
            ++stage;
        } else if (loaded) {
            event->type = GR_EVENT_TYPE_CLOSE_REQ;
            fputs("NETSURF_SCROLL_CHECK finished\n", stderr);
        } else goto ordinary;
        handle_event(event);
        return;
    }
ordinary:
    return;
}
int main(int argc, char **argv) { schedule(80,scripted_event,NULL);return browser_main(argc, argv); }
