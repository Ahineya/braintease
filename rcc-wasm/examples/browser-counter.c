#include "browser.h"
extern int puts(const char *);
static unsigned count;

static unsigned string(const char *text, unsigned cells) {
    return ripple_dom_string((const unsigned short *)text, cells);
}
static void render(void) {
    char text[5];
    text[0] = '0' + count / 1000;
    text[1] = '0' + count / 100 % 10;
    text[2] = '0' + count / 10 % 10;
    text[3] = '0' + count % 10; text[4] = 0;
    ripple_dom_begin();
    ripple_dom_command(RIPPLE_DOM_SET_TEXT, 4, string(text, 4), 0, 0);
    ripple_dom_commit();
}
int main(void) {
    unsigned div, button, greeting;
    ripple_browser_mount();
    ripple_dom_begin();
    div = string("div", 3); button = string("button", 6);
    greeting = string("Hello from Wasm", 15);
    ripple_dom_command(RIPPLE_DOM_ELEMENT, 1, div, 0, 0);
    ripple_dom_command(RIPPLE_DOM_TEXT, 2, greeting, 0, 0);
    ripple_dom_command(RIPPLE_DOM_ELEMENT, 3, button, 0, 0);
    ripple_dom_command(RIPPLE_DOM_TEXT, 4, string("0000", 4), 0, 0);
    ripple_dom_command(RIPPLE_DOM_INSERT, 0, 1, 0, 0);
    ripple_dom_command(RIPPLE_DOM_INSERT, 1, 2, 0, 0);
    ripple_dom_command(RIPPLE_DOM_INSERT, 1, 3, 0, 0);
    ripple_dom_command(RIPPLE_DOM_INSERT, 3, 4, 0, 0);
    ripple_dom_command(RIPPLE_DOM_LISTEN, 3, RIPPLE_EVENT_CLICK, 1, RIPPLE_PREVENT_DEFAULT);
    ripple_dom_commit();
    puts("Counter ready");
    return 0;
}
void ripple_browser_on_events(const unsigned short *data, unsigned long cells) {
    unsigned long p = 4;
    unsigned changed = 0;
    while (p < cells) {
        if (data[p+1] == 1 && data[p+3] == RIPPLE_EVENT_CLICK) {
            count = (count + 1) % 10000; changed = 1;
        }
        p += data[p];
    }
    if (changed) render();
}
void ripple_browser_on_dispose(void) { puts("Counter disposed"); }
