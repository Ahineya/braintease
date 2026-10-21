/* The Riplix scheduler owns process selection. These snapshots only preserve
 * the private ABI state, never a host/native return address or a run queue. */
static Process *process_allocate(void) { return (void *)pages_alloc((sizeof(Process) + 4095) / 4096); }
static void ram_release(RamPage *p) {
    if (p && !--p->references) page_free((uintptr_t)p);
}
static void process_release(Process *p) {
    if (p->loading) pages_release(p->loading, p->loading_size);
    jumps_release(p->jumps);
    for (unsigned i = 0; i < PRIVATE_PAGES; ++i) ram_release(p->pages[i]);
    pages_release(p, sizeof(*p));
}
static int process_save(Context *c, Process *p) {
    for (unsigned i = 0; i < PRIVATE_PAGES; ++i) if (c->dirty[i]) {
        uint16_t *source = c->cells + (size_t)i * PAGE_CELLS;
        unsigned nonzero = 0;
        for (unsigned j = 0; j < PAGE_CELLS; ++j) nonzero |= source[j];
        RamPage *page = p->pages[i];
        if (!nonzero) { ram_release(page); p->pages[i] = NULL; }
        else {
            if (!page || page->references != 1) {
                RamPage *next = (void *)page_alloc();
                if (!next) return 0;
                next->references = 1; ram_release(page); page = p->pages[i] = next;
            }
            memcpy(page->cells, source, sizeof(page->cells));
        }
        c->dirty[i] = 0;
    }
    p->depth = c->depth; p->stack = c->stack; p->errno_address = c->errno_address;
    memcpy(p->frames, c->frames, c->depth * sizeof(Frame));
    memcpy(&p->heap, &c->heap, sizeof(Heap));
    return 1;
}
static void process_restore(Context *c, Process *previous, Process *next) {
    for (unsigned i = 0; i < PRIVATE_PAGES; ++i) {
        if (previous->pages[i] == next->pages[i]) continue;
        uint16_t *target = c->cells + (size_t)i * PAGE_CELLS;
        if (next->pages[i]) memcpy(target, next->pages[i]->cells, PAGE_CELLS * 2);
        else memset(target, 0, PAGE_CELLS * 2);
    }
    c->depth = next->depth; c->stack = next->stack; c->errno_address = next->errno_address;
    memcpy(c->frames, next->frames, c->depth * sizeof(Frame));
    memcpy(&c->heap, &next->heap, sizeof(Heap));
    c->argc = 0;
}
static void process_clone(Context *c, U slot, U result) {
    if (result > SLOTS) fail("clone result slot", result);
    if (!slot || slot >= PROCESSES || slot == c->current || c->processes[slot]) {
        result_slot(c, result, 65535); return;
    }
    Process *child = process_allocate(), *parent = c->processes[c->current];
    if (!child) { result_slot(c, result, 65535); return; }
    if (!process_save(c, parent)) {
        pages_release(child, sizeof(*child)); result_slot(c, result, 65535); return;
    }
    memcpy(child, parent, sizeof(*child));
    child->loading = NULL; child->loading_size = child->loading_done = 0; child->staged = NULL;
    if (!jumps_clone(parent->jumps, &child->jumps)) {
        pages_release(child, sizeof(*child)); result_slot(c, result, 65535); return;
    }
    for (unsigned i = 0; i < PRIVATE_PAGES; ++i)
        if (child->pages[i]) ++child->pages[i]->references;
    if (result) child->frames[child->depth - 1].slots[result - 1] = 0;
    c->processes[slot] = child;
    result_slot(c, result, 1); ++c->forks;
}
static void process_switch(Context *c, U slot, U result, unsigned discard) {
    if (slot >= PROCESSES || !c->processes[slot]) fail("absent kernel context", slot);
    if (slot == c->current) {
        if (discard) fail("context cannot discard and resume itself", slot);
        result_slot(c, result, 0); return;
    }
    result_slot(c, result, 0);
    Process *old = c->processes[c->current], *next = c->processes[slot];
    /* Reconcile even a discarded image so pointer equality during restore
     * never hides its unsaved writes. */
    if (!process_save(c, old)) fail("context snapshot exhausted physical RAM", slot);
    unsigned previous = c->current;
    process_restore(c, old, next);
    c->current = slot; ++c->switches;
    if (discard) { c->processes[previous] = NULL; process_release(old); images_collect(c); ++c->drops; }
}
static void process_exec(Context *c, U token, U argument) {
    rcc_native_function(c, token, 1);
    Process *p = c->processes[c->current];
    Module *owner = c->functions[token_index(token)].owner;
    if (owner && p->staged != owner) fail("exec image was not staged", token);
    if (p->module) image_clear(c, p->module);
    memcpy(c->cells + BANK, c->global_template, sizeof(c->global_template));
    dirty(c, BANK, BANK);
    if (c->extended_cells) {
        memcpy(c->cells + 32U * BANK, c->extended_template, c->extended_cells * 2);
        dirty(c, 32U * BANK, c->extended_cells);
    }
    memset(c->cells + 2U * BANK, 0, 3U * BANK * 2);
    dirty(c, 2U * BANK, 3U * BANK);
    memcpy(&c->heap, &c->initial_heap, sizeof(Heap));
    c->stack = 2U * BANK + 1; c->depth = 0; c->argc = 0; c->errno_address = 0;
    p->module = owner; p->staged = NULL;
    jumps_release(p->jumps); p->jumps = NULL;
    if (owner) image_apply(c, owner);
    images_collect(c);
    rcc_native_process_argument(c, 0, argument);
    rcc_native_process_call(c, token, 1, 0);
    ++c->execs;
#ifdef RIPLIX_BOOT
    if (c->execs == 2) {
#ifdef PERSISTENT_ROOT
        screen_status("Riplix Bourne shell running. Disk files survive reboot; use shutdown.");
#elif defined(BOOT_NVME_PROBE)
        extern const char *nvme_probe_status(void);
        screen_status(nvme_probe_status());
#else
        screen_status("Riplix Bourne shell running. RAM files reset at reboot.");
#endif
    }
#endif
}
U rcc_native_ext_rcc_hosted_valid_function(Context *c, U token, U arity) {
    U index = (uint32_t)token ^ 0x80000000U;
    return index && index < FUNCTIONS && c->functions[index].address && c->functions[index].arity == arity;
}
