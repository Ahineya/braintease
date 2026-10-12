#include <graphics.h>

#ifndef RENDER_WIDTH
#define RENDER_WIDTH 4
#endif

/* Exercise the public API and separately compiled runtime, including helpers
   that are unused by this program and should not survive link reachability. */
int main(void) {
    graphics_init(RENDER_WIDTH, 2);
    clear_screen(rgb565(10, 10, 30));
    graphics_flush();
    return 0;
}
