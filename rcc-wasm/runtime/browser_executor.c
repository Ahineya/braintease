/* Passive browser reactor: one sampled clock and bounded guest turn per entry. */
extern void __wasm_trap(int);
extern unsigned ripple_browser_alive(void);
extern unsigned long ripple_browser_on_turn(void);
extern void ripple_browser_schedule(unsigned long);
static unsigned enabled, sampled;
static unsigned long clock_ms;
void ripple_browser_executor(void) {
    if (enabled || !ripple_browser_alive()) __wasm_trap(8);
    enabled = 1;
}
void __wasm_browser_clock(unsigned long now) {
    if (sampled && now - clock_ms > 2147483647UL) __wasm_trap(8);
    sampled = 1; clock_ms = now;
}
unsigned long ripple_browser_now(void) { return clock_ms; }
void __wasm_browser_turn(void) {
    if (!enabled || !ripple_browser_alive()) __wasm_trap(8);
    ripple_browser_schedule(ripple_browser_on_turn());
}
