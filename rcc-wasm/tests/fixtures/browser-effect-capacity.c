extern int putchar(int);
int main(void) {
    unsigned long ids[64], next; unsigned i, ok = 1;
    ripple_browser_mount(); ripple_browser_effects();
    for (i = 0; i < 64; ++i) {
        ids[i] = ripple_effect_timer(0);
        if (!ids[i] || ripple_effect_state(ids[i])) ok = 0;
    }
    if (ripple_effect_timer(0)) ok = 0;
    for (i = 0; i < 64; ++i) { ripple_effect_release(ids[i]); ripple_effect_release(ids[i]); }
    next = ripple_effect_timer(0);
    if (next <= ids[63] || ripple_effect_state(ids[0]) != 5) ok = 0;
    ripple_effect_release(next);
    putchar(ok ? 'Y' : 'N'); return 0;
}
void ripple_browser_on_events(const unsigned short *p, unsigned long n) {}
void ripple_browser_on_effects(void) { putchar('N'); }
void ripple_browser_on_dispose(void) {}
