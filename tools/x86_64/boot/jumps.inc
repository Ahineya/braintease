/* Save guest continuations, never the native machine stack. Memory writes
 * after setjmp remain visible; frame identities reject targets that returned. */
struct Jump {
    Jump *next;
    Frame frame;
    U cookie, address, result;
    size_t depth, stack;
};
static void jumps_release(Jump *jump) {
    while (jump) {
        Jump *next = jump->next;
        pages_release(jump, sizeof(*jump)); jump = next;
    }
}
static int jumps_clone(Jump *source, Jump **result) {
    *result = NULL;
    Jump **tail = result;
    for (; source; source = source->next) {
        Jump *next = (void *)pages_alloc((sizeof(Jump) + 4095) / 4096);
        if (!next) { jumps_release(*result); *result = NULL; return 0; }
        memcpy(next, source, sizeof(*next)); next->next = NULL;
        *tail = next; tail = &next->next;
    }
    return 1;
}
static void jumps_prune(Context *c) {
    Jump **at = &c->processes[c->current]->jumps;
    while (*at) {
        Jump *j = *at;
        if (j->depth <= c->depth && c->frames[j->depth - 1].id == j->frame.id) at = &j->next;
        else { *at = j->next; pages_release(j, sizeof(*j)); }
    }
}
static void process_jump(Context *c, U op, U address, U value, U result) {
    size_t at = span(address, 4);
    if (at < BANK || at > KERNEL_START - 4) fail("invalid jump buffer", address);
    Process *p = c->processes[c->current];
    if (op == 13) {
        Jump **link = &p->jumps;
        unsigned count = 0;
        while (*link) {
            if ((*link)->address == address) {
                Jump *old = *link; *link = old->next; pages_release(old, sizeof(*old)); break;
            }
            ++count; link = &(*link)->next;
        }
        if (count >= 64) fail("saved jump capacity", count);
        Jump *j = (void *)pages_alloc((sizeof(Jump) + 4095) / 4096);
        if (!j) fail("saved jump allocation", sizeof(Jump));
        result_slot(c, result, 0);
        j->frame = *frame(c); j->cookie = ++c->next_jump; j->address = address;
        j->result = result; j->depth = c->depth; j->stack = c->stack;
        j->next = p->jumps; p->jumps = j;
        rcc_native_store(c, address, j->cookie, 4);
    } else {
        U cookie = rcc_native_load(c, address, 4);
        Jump *j = p->jumps;
        while (j && j->cookie != cookie) j = j->next;
        if (!j || j->depth > c->depth || c->frames[j->depth - 1].id != j->frame.id)
            fail("expired jump target", cookie);
        c->depth = j->depth; c->frames[c->depth - 1] = j->frame; c->stack = j->stack;
        result_slot(c, j->result, (uint16_t)value ? (uint16_t)value : 1);
        c->argc = 0; jumps_prune(c);
    }
}
