/* Compare the demo's incremental painter with its full repaint path.
 * Both paths compile the real demo source and use upstream SDL surfaces.
 * No second implementation of the scene or its geometry lives in this test.
 */
#include <SDL.h>
#include <assert.h>

#define MAX_DAMAGE (8 * 6 + 2)
static int presents, last_count;
static unsigned long last_pixels;
static SDL_Rect last_damage[MAX_DAMAGE];
static void checked_updates(SDL_Surface *surface, int count, SDL_Rect *rects);

#define SDL_UpdateRects checked_updates
#define main demo_main
#include "demo.c"
#undef main
#undef SDL_UpdateRects

static void checked_updates(SDL_Surface *surface, int count, SDL_Rect *rects)
{
    int i;
    assert(count > 0 && count <= MAX_DAMAGE);
    presents++;
    last_count = count;
    last_pixels = 0;
    for (i = 0; i < count; i++) {
        assert(rects[i].x >= 0 && rects[i].y >= 0);
        assert(rects[i].w > 0 && rects[i].h > 0);
        assert(rects[i].x + rects[i].w <= surface->w);
        assert(rects[i].y + rects[i].h <= surface->h);
        last_damage[i] = rects[i];
        last_pixels += (unsigned long)rects[i].w * rects[i].h;
    }
    SDL_UpdateRects(surface, count, rects);
}

static int covered(int x, int y)
{
    int i;
    for (i = 0; i < last_count; i++) {
        SDL_Rect *r = &last_damage[i];
        if (x >= r->x && x < r->x + r->w && y >= r->y && y < r->y + r->h)
            return 1;
    }
    return 0;
}

static void compare(int repaint, int expect_full)
{
    size_t size = (size_t)screen->pitch * screen->h;
    unsigned char *before = malloc(size), *partial = malloc(size);
    int calls = presents, x, y, bytes = screen->format->BytesPerPixel;
    assert(before && partial);
    memcpy(before, screen->pixels, size);
    assert(draw(repaint));
    /* Every application frame presents once, including several dirty cells. */
    assert(presents == calls + 1);
    if (expect_full) {
        assert(last_count == 1);
        assert(last_pixels == (unsigned long)screen->w * screen->h);
    } else {
        assert(last_pixels < (unsigned long)screen->w * screen->h);
    }
    memcpy(partial, screen->pixels, size);
    for (y = 0; y < screen->h; y++) {
        for (x = 0; x < screen->w; x++) {
            size_t offset = (size_t)y * screen->pitch + (size_t)x * bytes;
            if (memcmp(before + offset, partial + offset, bytes))
                assert(covered(x, y));
        }
    }
    /* Repaint the same state from scratch, rather than reproducing its code. */
    assert(draw(1));
    assert(!memcmp(partial, screen->pixels, size));
    free(before);
    free(partial);
}

static void unchanged(void)
{
    int calls = presents;
    assert(!draw(0));
    assert(presents == calls);
}

int main(void)
{
    const int depths[] = {16, 24, 32};
    const int sizes[][2] = {{320, 240}, {641, 403}, {1024, 1024}};
    int d, s, i, comparisons = 0, noops = 0;
    assert(SDL_Init(SDL_INIT_VIDEO) == 0);
    for (d = 0; d < 3; d++) {
        depth = depths[d];
        for (s = 0; s < 3; s++) {
            video(sizes[s][0], sizes[s][1]);
            selected_x = selected_y = tint = 0;
            /* Mode changes invalidate the entire image, including margins. */
            compare(1, 1);
            comparisons++;
            for (i = 0; i < 48; i++) {
                selected_x = (selected_x + 1) % 8;
                if (!selected_x)
                    selected_y = (selected_y + 1) % 6;
                tint = (selected_x + selected_y) & 1;
                compare(0, 0);
                comparisons++;
                unchanged();
                noops++;
            }
            /* A palette change must preserve the selected outline and bars. */
            tint = !tint;
            compare(0, 0);
            comparisons++;
            /* Exercise reverse navigation and both wrapping directions. */
            selected_x = (selected_x + 7) % 8;
            compare(0, 0);
            selected_x = (selected_x + 1) % 8;
            compare(0, 0);
            selected_y = (selected_y + 5) % 6;
            compare(0, 0);
            selected_y = (selected_y + 1) % 6;
            compare(0, 0);
            comparisons += 4;
            unchanged();
            noops++;
            /* Explicit expose and benchmark mode also repaint unchanged state. */
            compare(1, 1);
            full_redraw = 1;
            compare(0, 1);
            full_redraw = 0;
            comparisons += 2;
        }
        /* A changed size requests a full frame even without an expose flag. */
        video(777, 555);
        compare(0, 1);
        comparisons++;
    }
    SDL_Quit();
    printf("SDL_DEMO_DAMAGE_OK comparisons=%d noops=%d\n", comparisons, noops);
    return 0;
}
