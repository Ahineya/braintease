#ifndef DOOM_RESOLUTION_H
#define DOOM_RESOLUTION_H

/* Keep Doom's projection/UI coordinates and indexed row stride at 320x200.
 * Only pixels whose absolute x and y are multiples of this factor are drawn;
 * presentation packs those samples into a smaller RGB565 framebuffer. */
#ifndef DOOM_RESOLUTION_SCALE
#define DOOM_RESOLUTION_SCALE 1
#endif
#if DOOM_RESOLUTION_SCALE != 1 && DOOM_RESOLUTION_SCALE != 2 && DOOM_RESOLUTION_SCALE != 4
#error DOOM_RESOLUTION_SCALE must be 1, 2 or 4
#endif
#define DOOM_OUTPUT_WIDTH (320 / DOOM_RESOLUTION_SCALE)
#define DOOM_OUTPUT_HEIGHT (200 / DOOM_RESOLUTION_SCALE)

#endif
