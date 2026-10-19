/* Browser effect ABI v1. Operation IDs never wrap/reuse during one mount.
 * Registrations and cancellations are staged in Wasm and flushed per turn. */
typedef unsigned short cell;
extern void __wasm_trap(int);
extern void __wasm_effect_commit(const cell *, unsigned long);
extern unsigned ripple_browser_alive(void);
extern void ripple_browser_on_effects(void);
extern void *malloc(unsigned long);
extern void free(void *);
struct effect { unsigned long id; unsigned kind, state, status, length, limit; cell *body; };
static struct effect jobs[64];
static cell outgoing[32768], incoming[32768];
static unsigned used = 8, commands, enabled;
static unsigned long serial;
static unsigned long wake_hint = 0xffffffffUL;
static unsigned hint_dirty;
void ripple_browser_schedule(unsigned long delay) {
    if (delay != 0xffffffffUL && delay > 2147483647UL) __wasm_trap(8);
    if (delay != 0xffffffffUL || delay != wake_hint) hint_dirty = 1;
    wake_hint = delay;
}
static struct effect *find(unsigned long id) {
    unsigned i;
    if (!id) return 0;
    for (i = 0; i < 64; ++i) if (jobs[i].id == id) return &jobs[i];
    return 0;
}
void ripple_browser_effects(void) {
    if (!ripple_browser_alive() || enabled) __wasm_trap(8);
    enabled = 1;
}
static void command(unsigned op, unsigned long id, unsigned long arg, const cell *text, unsigned length) {
    unsigned i, p = used;
    if (commands == 256 || 8UL + length > 32768UL - used) __wasm_trap(8);
    outgoing[p] = 8 + length; outgoing[p+1] = op;
    outgoing[p+2] = (unsigned)id; outgoing[p+3] = (unsigned)(id >> 16);
    outgoing[p+4] = (unsigned)arg; outgoing[p+5] = (unsigned)(arg >> 16);
    outgoing[p+6] = length; outgoing[p+7] = 0;
    for (i = 0; i < length; ++i) outgoing[p+8+i] = text[i];
    used += 8 + length; ++commands;
}
static unsigned long begin(unsigned kind, unsigned long arg, const cell *text, unsigned length) {
    unsigned i;
    if (!enabled || !ripple_browser_alive()) __wasm_trap(8);
    if (serial == 0xffffffffUL) return 0;
    for (i = 0; i < 64; ++i) if (!jobs[i].id) {
        jobs[i].id = ++serial; jobs[i].kind = kind; jobs[i].state = 0;
        jobs[i].status = 0; jobs[i].length = 0; jobs[i].limit = kind == 2 || kind == 6 ? (unsigned)arg : kind == 4 ? 2048 : 0; jobs[i].body = 0;
        command(kind, serial, arg, text, length); return serial;
    }
    return 0;
}
unsigned long ripple_effect_timer(unsigned long milliseconds) {
    if (milliseconds > 2147483647UL) __wasm_trap(8);
    return begin(1, milliseconds, 0, 0);
}
unsigned long ripple_effect_get(const cell *url, unsigned long length, unsigned long limit) {
    unsigned i;
    if (!length || length > 2048 || !limit || limit > 16384) __wasm_trap(8);
    for (i = 0; i < length; ++i) if (!url[i] || url[i] > 255) __wasm_trap(8);
    return begin(2, limit, url, (unsigned)length);
}
/* Platform operations share the bounded effect wire and cancellation table. */
unsigned long ripple_effect_platform(unsigned kind, unsigned long arg, const cell *text, unsigned long length) {
    unsigned i;
    if (kind < 4 || kind > 8 || length > 16640) __wasm_trap(8);
    if ((kind == 4 && (arg || length)) ||
        (kind == 5 && (arg > 1 || !length || length > 2048)) ||
        (kind == 6 && (!arg || arg > 16384 || !length || length > 256)) ||
        (kind == 7 && (!arg || arg > 256 || arg > length || length - arg > 16384)) ||
        (kind == 8 && (arg || !length || length > 256))) __wasm_trap(8);
    for (i = 0; i < length; ++i) if (text[i] > 255 || (!text[i] && (kind != 7 || i < arg))) __wasm_trap(8);
    return begin(kind, arg, text, (unsigned)length);
}
void ripple_effect_consume(unsigned long id) {
    struct effect *job = find(id);
    if (!job || job->kind != 4 || !job->state) __wasm_trap(8);
    free(job->body); job->body = 0; job->length = 0; job->state = 0;
}
unsigned ripple_effect_state(unsigned long id) {
    struct effect *job = find(id); return job ? job->state : 5;
}
unsigned ripple_effect_status(unsigned long id) {
    struct effect *job = find(id); return job ? job->status : 0;
}
unsigned ripple_effect_length(unsigned long id) {
    struct effect *job = find(id); return job ? job->length : 0;
}
void ripple_effect_copy(unsigned long id, cell *destination, unsigned long capacity) {
    struct effect *job = find(id); unsigned i;
    if (!job || job->state != 1 || capacity < job->length) __wasm_trap(8);
    for (i = 0; i < job->length; ++i) destination[i] = job->body[i];
}
void ripple_effect_release(unsigned long id) {
    struct effect *job = find(id);
    if (!job) return;
    /* A delivered completion already retired the host registration. */
    if (!job->state || job->kind == 4) command(3, id, 0, 0, 0);
    free(job->body); job->body = 0; job->id = 0;
}
cell *__wasm_effect_inbox(void) { return incoming; }
void __wasm_effect_flush(void) {
    if (!commands && !hint_dirty) return;
    outgoing[0] = 0x5246; outgoing[1] = 1; outgoing[2] = used; outgoing[3] = commands;
    outgoing[4] = hint_dirty; outgoing[5] = hint_dirty ? (unsigned)wake_hint : 0;
    outgoing[6] = hint_dirty ? (unsigned)(wake_hint >> 16) : 0; outgoing[7] = 0;
    __wasm_effect_commit(outgoing, (unsigned long)used);
    used = 8; commands = 0; hint_dirty = 0;
}
void __wasm_effect_deliver(unsigned long cells) {
    unsigned long p = 4, i, j, id; unsigned length, error, kind, status; struct effect *job;
    if (!enabled || !ripple_browser_alive() || cells < 4 || cells > 32768 ||
        incoming[0] != 0x5247 || incoming[1] != 1 || incoming[2] != cells || incoming[3] > 64)
        __wasm_trap(8);
    /* Validate the entire batch before changing any operation state. */
    for (i = 0; i < incoming[3]; ++i) {
        if (p + 8 > cells) __wasm_trap(8);
        length = incoming[p+5]; error = incoming[p+6]; kind = incoming[p+3]; status = incoming[p+4];
        if (length > 16384 || incoming[p] != 8UL + length || p + 8 + length > cells ||
            (!incoming[p+1] && !incoming[p+2]) || kind < 1 || kind > 8 || kind == 3 ||
            status > 599 || error > 4 || incoming[p+7] ||
            (error && (length || incoming[p+4])) ||
            (incoming[p+3] == 1 && (length || incoming[p+4] || error)) ||
            (kind == 2 && (error > 2 || (!error && status < 100))) ||
            (kind == 4 && (length > 2048 || status || (error && error != 2 && error != 3))) ||
            (kind == 5 && (length || status || (error && error != 3))) ||
            (kind == 6 && (status > 1 || (!status && length))) ||
            ((kind == 7 || kind == 8) && (length || status))) __wasm_trap(8);
        id = (unsigned long)incoming[p+1] | ((unsigned long)incoming[p+2] << 16);
        job = find(id);
        if (job && (!job->state || job->kind == 4) && (job->kind != incoming[p+3] || length > job->limit)) __wasm_trap(8);
        for (j = 0; j < length; ++j) if (incoming[p+8+j] > 255) __wasm_trap(8);
        p += 8UL + length;
    }
    if (p != cells) __wasm_trap(8);
    p = 4;
    for (i = 0; i < incoming[3]; ++i) {
        id = (unsigned long)incoming[p+1] | ((unsigned long)incoming[p+2] << 16);
        length = incoming[p+5]; error = incoming[p+6]; job = find(id);
        if (job && (!job->state || job->kind == 4)) {
            if (job->kind != incoming[p+3]) __wasm_trap(8);
            free(job->body); job->body = 0;
            if (length) {
                job->body = malloc((unsigned long)length);
                if (!job->body) __wasm_trap(4);
                for (j = 0; j < length; ++j) job->body[j] = incoming[p+8+j];
            }
            job->length = length; job->status = incoming[p+4]; job->state = error == 4 ? 6 : 1 + error;
        }
        p += 8UL + length;
    }
    ripple_browser_on_effects();
}
void __wasm_effect_shutdown(void) {
    unsigned i;
    for (i = 0; i < 64; ++i) { free(jobs[i].body); jobs[i].body = 0; jobs[i].id = 0; }
    used = 8; commands = 0; enabled = 0; hint_dirty = 0; wake_hint = 0xffffffffUL;
}
