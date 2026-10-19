/* Copy-only bridge between compatibility glyph IDs and host curses glyph IDs.
 * Scalars retain their value; interned grapheme clusters are mapped once. */
#include <ripple_unicode.h>
unsigned long rfl_host_glyph(const unsigned long *, unsigned);
int rfl_host_cells(void *, int, int, const unsigned long *, int);
static unsigned long mapped[RU_CLUSTER_LIMIT];
int rfl_curses_cells(void *window, int row, int column, const unsigned long *cells, int count) {
    unsigned long translated[512], scalars[RU_CLUSTER_SCALARS], cp, id;
    unsigned n, j; int i;
    if (count < 0 || count > 512 || (count && !cells)) return -1;
    for (i = 0; i < count; ++i) {
        cp = cells[i] & 0x1fffffUL;
        if (cp >= RU_CLUSTER_BASE && cp < RU_CLUSTER_BASE + RU_CLUSTER_LIMIT) {
            id = cp - RU_CLUSTER_BASE;
            if (!mapped[id]) {
                n = ru_scalar_count(cp);
                if (!n || n > RU_CLUSTER_SCALARS) return -1;
                for (j = 0; j < n; ++j) scalars[j] = ru_scalar(cp, j);
                mapped[id] = rfl_host_glyph(scalars, n);
            }
            cp = mapped[id];
        }
        translated[i] = (cells[i] & 0xffe00000UL) | cp;
    }
    return rfl_host_cells(window, row, column, translated, count);
}
