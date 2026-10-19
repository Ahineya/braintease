extern int putchar(int);
static unsigned long first, second, cancelled, failed, large;
static unsigned turns;
int main(void) {
    ripple_browser_mount(); ripple_browser_effects();
    ripple_dom_begin();
    ripple_dom_command(1, 1, ripple_dom_string((const unsigned short *)"button", 6), 0, 0);
    ripple_dom_command(6, 0, 1, 0, 0);
    ripple_dom_command(8, 1, 1, 1, 0);
    ripple_dom_commit();
    first = ripple_effect_timer(10);
    second = ripple_effect_get((const unsigned short *)"./ok", 4, 16);
    cancelled = ripple_effect_get((const unsigned short *)"./cancel", 8, 16);
    failed = ripple_effect_get((const unsigned short *)"./fail", 6, 16);
    large = ripple_effect_get((const unsigned short *)"./large", 7, 4);
    return 0;
}
void ripple_browser_on_events(const unsigned short *data, unsigned long cells) {
    ripple_effect_release(cancelled); /* Cancel while fetch/its read is pending. */
}
void ripple_browser_on_effects(void) {
    unsigned short body[16];
    ++turns;
    if (ripple_effect_state(first) == 1) { putchar('T'); ripple_effect_release(first); }
    if (ripple_effect_state(second) == 1) {
        ripple_effect_copy(second, body, 16);
        if (ripple_effect_status(second) == 404 && ripple_effect_length(second) == 5 &&
            body[0] == 104 && body[1] == 195 && body[2] == 169 && body[3] == 108 && body[4] == 111) putchar('F');
        else putchar('N');
        ripple_effect_release(second);
    }
    if (ripple_effect_state(failed) == 2) { putchar('E'); ripple_effect_release(failed); }
    if (ripple_effect_state(large) == 3) { putchar('L'); ripple_effect_release(large); }
}
void ripple_browser_on_dispose(void) {
    ripple_effect_release(first); ripple_effect_release(second); ripple_effect_release(cancelled);
    ripple_effect_release(failed); ripple_effect_release(large); putchar('D');
}
