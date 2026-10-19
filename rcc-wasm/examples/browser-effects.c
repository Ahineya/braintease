#include "browser.h"
extern int putchar(int);
extern int puts(const char *);
static unsigned long timer, request;
static unsigned timer_done, request_done, cancelled;
static unsigned text(const char *s) {
    unsigned n = 0;
    while (s[n]) ++n;
    return ripple_dom_string((const unsigned short *)s, n);
}
static void render(void) {
    const char *message = cancelled ? "Cancelled." :
        timer_done && request_done ? "Timer and fetch completed inside Wasm." :
        request_done ? "Fetch complete; waiting for timer..." :
        timer_done ? "Timer complete; waiting for fetch..." : "Waiting for timer and fetch...";
    ripple_dom_begin();
    ripple_dom_command(RIPPLE_DOM_SET_TEXT, 2, text(message), 0, 0);
    ripple_dom_commit();
}
int main(void) {
    ripple_browser_mount(); ripple_browser_effects();
    ripple_dom_begin();
    ripple_dom_command(RIPPLE_DOM_ELEMENT, 1, text("p"), 0, 0);
    ripple_dom_command(RIPPLE_DOM_TEXT, 2, text("Waiting for timer and fetch..."), 0, 0);
    ripple_dom_command(RIPPLE_DOM_ELEMENT, 3, text("button"), 0, 0);
    ripple_dom_command(RIPPLE_DOM_TEXT, 4, text("Cancel work"), 0, 0);
    ripple_dom_command(RIPPLE_DOM_INSERT, 0, 1, 0, 0);
    ripple_dom_command(RIPPLE_DOM_INSERT, 1, 2, 0, 0);
    ripple_dom_command(RIPPLE_DOM_INSERT, 0, 3, 0, 0);
    ripple_dom_command(RIPPLE_DOM_INSERT, 3, 4, 0, 0);
    ripple_dom_command(RIPPLE_DOM_LISTEN, 3, RIPPLE_EVENT_CLICK, 1, 0);
    ripple_dom_commit();
    timer = ripple_effect_timer(1000);
    request = ripple_effect_get((const unsigned short *)"./message.txt", 13, 1024);
    puts("Timer and fetch started in one batch.");
    return 0;
}
void ripple_browser_on_events(const unsigned short *data, unsigned long cells) {
    unsigned long p = 4;
    for (; p < cells; p += data[p]) if (data[p+1] == 1) {
        ripple_effect_release(timer); ripple_effect_release(request);
        if (!cancelled) { cancelled = 1; render(); puts("Work cancelled."); }
    }
}
void ripple_browser_on_effects(void) {
    unsigned state, i, n; unsigned short body[1024]; unsigned changed = 0;
    if (!timer_done && ripple_effect_state(timer) == 1) {
        timer_done = 1; changed = 1; ripple_effect_release(timer); puts("Timer completed.");
    }
    state = ripple_effect_state(request);
    if (!request_done && state > 0 && state < 5) {
        request_done = 1; changed = 1;
        if (state == 1) {
            n = ripple_effect_length(request); ripple_effect_copy(request, body, 1024);
            for (i = 0; i < n; ++i) putchar(body[i]);
        } else puts("Fetch failed or exceeded its body limit.");
        ripple_effect_release(request);
    }
    if (changed && !cancelled) render();
}
void ripple_browser_on_dispose(void) {
    ripple_effect_release(timer); ripple_effect_release(request); puts("Browser effects disposed.");
}
