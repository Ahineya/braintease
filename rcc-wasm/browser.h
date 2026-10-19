#ifndef RIPPLE_WASM_BROWSER_H
#define RIPPLE_WASM_BROWSER_H
/* Low-level browser ABI. See docs/rfl/browser-bridge.md for the wire protocol.
 * Call mount once from main; provide both hooks, even if disposal is empty.
 * A hook must not retain the borrowed inbox or its pointers. */
void ripple_browser_mount(void);
void ripple_browser_on_events(const unsigned short *data, unsigned long cells);
void ripple_browser_on_dispose(void);
void ripple_dom_begin(void);
void ripple_dom_abort(void);
unsigned ripple_dom_string(const unsigned short *text, unsigned long cells);
void ripple_dom_command(unsigned op, unsigned a, unsigned b, unsigned c, unsigned d);
void ripple_dom_commit(void);

/* Optional effects ABI: enable once after mount, implement the completion hook.
 * Starts/releases are batched at the end of start/events/completions turns.
 * State: 0 pending, 1 success, 2 network error, 3 body limit,
 * 4 unavailable browser service, 5 retired, 6 quota exceeded.
 * Release cancels unfinished work, frees response storage, and is idempotent. */
void ripple_browser_effects(void);
void ripple_browser_on_effects(void);
unsigned long ripple_effect_timer(unsigned long milliseconds);
unsigned long ripple_effect_get(const unsigned short *url, unsigned long length, unsigned long limit);
/* 4 location subscription, 5 hash navigation, 6/7/8 storage read/write/remove.
 * See browser-routing.md for payload bounds, status and ownership rules. */
unsigned long ripple_effect_platform(unsigned kind, unsigned long arg, const unsigned short *text, unsigned long length);
/* Consumes one location snapshot and rearms the same subscription in guest memory. */
void ripple_effect_consume(unsigned long id);
unsigned ripple_effect_state(unsigned long id);
unsigned ripple_effect_status(unsigned long id);
unsigned ripple_effect_length(unsigned long id);
void ripple_effect_copy(unsigned long id, unsigned short *destination, unsigned long capacity);
void ripple_effect_release(unsigned long id);

/* Optional mounted executor: enable once after mount/effects. The backend
 * samples a wrapping monotonic millisecond clock before each active entry,
 * then calls on_turn once. Return 0 for runnable work, a delay <= 0x7fffffff,
 * or 0xffffffff for no wake. The hint shares the outgoing effect batch. */
void ripple_browser_executor(void);
unsigned long ripple_browser_now(void);
unsigned long ripple_browser_on_turn(void);

#define RIPPLE_DOM_ELEMENT 1
#define RIPPLE_DOM_TEXT 2
#define RIPPLE_DOM_SET_TEXT 3
#define RIPPLE_DOM_ATTRIBUTE 4
#define RIPPLE_DOM_PROPERTY 5
#define RIPPLE_DOM_INSERT 6
#define RIPPLE_DOM_REMOVE 7
#define RIPPLE_DOM_LISTEN 8
#define RIPPLE_DOM_UNLISTEN 9
#define RIPPLE_DOM_REMOVE_ATTRIBUTE 10
#define RIPPLE_DOM_FOCUS 11
#define RIPPLE_EVENT_CLICK 1
#define RIPPLE_EVENT_INPUT 2
#define RIPPLE_EVENT_CHANGE 3
#define RIPPLE_EVENT_KEYDOWN 4
#define RIPPLE_EVENT_KEYUP 5
#define RIPPLE_EVENT_COMPOSITION_START 6
#define RIPPLE_EVENT_COMPOSITION_END 7
#define RIPPLE_EVENT_DOUBLE_CLICK 8
#define RIPPLE_EVENT_BLUR 9
#define RIPPLE_EVENT_FOCUS 10
#define RIPPLE_PREVENT_DEFAULT 1
#define RIPPLE_STOP_PROPAGATION 2
#endif
