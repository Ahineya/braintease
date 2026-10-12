#include <graphics.h>

/* Nested clipped loops, overlapping stores, empty rectangles and high unsigned
   coordinates. The independent expected image lives in test-bf-compiler.mjs. */
int main(void) {
    graphics_init(12, 8);
    clear_screen(RGB_BLACK);
    fill_rect(1, 1, 4, 3, RGB_RED);
    fill_rect(3, 2, 6, 3, RGB_GREEN);
    fill_rect(10, 6, 5, 5, RGB_BLUE);
    fill_rect(11, 0, 65535, 1, RGB_YELLOW);
    fill_rect(0, 7, 2, 65535, RGB_CYAN);
    fill_rect(0, 0, 0, 8, RGB_WHITE);
    fill_rect(0, 0, 12, 0, RGB_WHITE);
    fill_rect(12, 0, 1, 1, RGB_WHITE);
    fill_rect(0, 8, 1, 1, RGB_WHITE);
    /* The unchanged BF CPU miscompares an unsigned left operand of 65535.
       Its separate ignored bf_host regression records that existing limit. */
    fill_rect(65534, 0, 2, 1, RGB_WHITE);
    fill_rect(0, 65534, 1, 2, RGB_WHITE);
    graphics_flush();
    return 0;
}
