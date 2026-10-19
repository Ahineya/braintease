extern int putchar(int);
static unsigned long subscription;
int main(void) {
    ripple_browser_mount();
    ripple_browser_effects();
    subscription = ripple_effect_platform(4, 0, (const unsigned short *)0, 0);
    return 0;
}
void ripple_browser_on_events(const unsigned short *data, unsigned long cells) {}
void ripple_browser_on_effects(void) {
    unsigned short text[3];
    if (ripple_effect_state(subscription) != 1 || ripple_effect_length(subscription) != 3) {
        putchar('N'); return;
    }
    ripple_effect_copy(subscription, text, 3);
    putchar(text[0] == '#' && text[1] == '/' && text[2] == 'x' ? 'Y' : 'N');
    ripple_effect_consume(subscription);
}
void ripple_browser_on_dispose(void) { ripple_effect_release(subscription); }
