/* Browser wire ABI v1. Counts/offsets are C16 cells, not bytes. The host
 * borrows patch storage during commit and never synchronously reenters us. */
typedef unsigned short cell;
extern void __wasm_trap(int);
extern void __wasm_dom_commit(const cell *, unsigned long);
extern void ripple_browser_on_events(const cell *, unsigned long);
extern void ripple_browser_on_dispose(void);
static cell patch[32768], strings[16384], inbox[4096];
static unsigned commands, string_cells, building, mounted, disposed;

unsigned ripple_browser_alive(void) { return mounted && !disposed; }
void ripple_browser_mount(void) {
    if (mounted || disposed) __wasm_trap(8);
    mounted = 1;
}
void ripple_dom_begin(void) {
    if (!mounted || disposed || building) __wasm_trap(8);
    building = 1; commands = 0; string_cells = 0;
}
void ripple_dom_abort(void) { building = 0; commands = 0; string_cells = 0; }
unsigned ripple_dom_string(const cell *text, unsigned long cells) {
    unsigned id, i;
    if (!building || cells > 16383UL || cells + 1 > 16384UL - string_cells)
        __wasm_trap(8);
    id = string_cells;
    strings[string_cells++] = (unsigned)cells;
    for (i = 0; i < cells; ++i) {
        if (text[i] > 255) __wasm_trap(8);
        strings[string_cells++] = text[i];
    }
    return id;
}
void ripple_dom_command(unsigned op, unsigned a, unsigned b, unsigned c, unsigned d) {
    unsigned p;
    if (!building || commands == 1024) __wasm_trap(8);
    p = 8 + commands++ * 8;
    patch[p] = op; patch[p+1] = a; patch[p+2] = b;
    patch[p+3] = c; patch[p+4] = d;
    patch[p+5] = 0; patch[p+6] = 0; patch[p+7] = 0;
}
void ripple_dom_commit(void) {
    unsigned p, i;
    if (!building) __wasm_trap(8);
    building = 0;
    if (!commands) return;
    p = 8 + commands * 8;
    for (i = 0; i < string_cells; ++i) patch[p+i] = strings[i];
    patch[0] = 0x5244; patch[1] = 1; patch[2] = p + string_cells;
    patch[3] = commands; patch[4] = 8; patch[5] = p;
    patch[6] = string_cells; patch[7] = 0;
    __wasm_dom_commit(patch, (unsigned long)p + string_cells);
}
cell *__wasm_browser_inbox(void) { return inbox; }
const cell *ripple_browser_event_data(void) { return inbox; }
void __wasm_browser_events(unsigned long cells) {
    unsigned long p = 4, i, j, size;
    if (!mounted || disposed || building || cells < 4 || cells > 4096 ||
        inbox[0] != 0x5245 || inbox[1] != 1 || inbox[2] != cells)
        __wasm_trap(8);
    if (inbox[3] > 64) __wasm_trap(8);
    for (i = 0; i < inbox[3]; ++i) {
        if (p + 8 > cells) __wasm_trap(8);
        size = 8UL + inbox[p+5] + inbox[p+6];
        if (inbox[p] != size || p + size > cells || !inbox[p+1] ||
            !inbox[p+2] || inbox[p+3] < 1 || inbox[p+3] > 10 ||
            inbox[p+4] > 31 || inbox[p+7] > 1) __wasm_trap(8);
        for (j = p + 8; j < p + size; ++j) if (inbox[j] > 255) __wasm_trap(8);
        p += size;
    }
    if (p != cells) __wasm_trap(8);
    ripple_browser_on_events(inbox, cells);
    if (building) __wasm_trap(8);
}
void __wasm_browser_dispose(void) {
    if (disposed) return;
    disposed = 1; building = 0;
    if (mounted) ripple_browser_on_dispose();
}
