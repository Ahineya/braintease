#ifndef RENDER_WIDTH
#define RENDER_WIDTH 4
#endif
#ifndef RENDER_PIXELS
#define RENDER_PIXELS 8
#endif

/* Ordinary C: no inline assembly or host-side framebuffer shortcut. */
int main(void) {
    *(volatile unsigned short *)16 = (RENDER_WIDTH << 8) | 2;
    *(volatile unsigned short *)6 = 3;
    for (unsigned short i = 0; i < RENDER_PIXELS; ++i)
        ((volatile unsigned short *)(32 + RENDER_PIXELS))[i] = 0x0843;
    *(volatile unsigned short *)9 = 1;
    return 0;
}
