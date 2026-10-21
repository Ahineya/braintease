/* RXE1: explicit x86_64, static guest libc, no host ELF/DLL resolver.
 * Packages are streamed through Riplix readi before any process state changes.
 * Code stays resident while any live/parked process or staged exec owns it. */
#define IMAGES 128
#define IMAGE_MAX (32U * 1024 * 1024)
typedef struct {
    char magic[8];
    uint32_t tag, total, code_size, functions, helpers, relocations, initializers;
    uint32_t entry, global_bank, global_cells, error, bindings, reserved[2];
} ImageHeader;
typedef struct { uint32_t local, arity, offset, reserved; } ImageFunction;
typedef struct { char name[64]; uint32_t arity, reserved; } ImageHelper;
typedef struct { U offset, addend; } ImageRelocation;
typedef struct { uint32_t pointer, width; U value; uint32_t function, reserved; } ImageInitializer;
struct Module {
    uint8_t *package, *code;
    size_t size, global_start, global_cells;
    uint16_t *template;
    unsigned first, count, entry;
    U error;
};
static Module images[IMAGES];

static int image_string(const char *left, const char *right) {
    for (unsigned i = 0; i < 64; ++i) {
        if (left[i] != right[i]) return 0;
        if (!left[i]) return 1;
    }
    return 0;
}
U rcc_native_image_token(Context *c, U local) {
    Module *m = c->functions[token_index(frame(c)->token)].owner;
    unsigned index = (uint32_t)local ^ 0x80000000U;
    if (!m || !index || index >= m->count || c->functions[m->first + index].owner != m)
        fail("application local function token", local);
    return 0x80000000U | (m->first + index);
}
#ifdef BOOT_MATH
extern U rcc_native_ext___rcc_host_math(Context *, U, U, U);
extern U riplix_math_selftest(void);
#endif
static U image_helper(const char *name, unsigned arity) {
#define BIND(symbol, count) if (arity == count && image_string(name, #symbol)) return (uintptr_t)symbol
    BIND(rcc_native_poll, 0);
#ifdef BOOT_MATH
    if (arity == 3 && image_string(name, "rcc_native_ext___rcc_host_math")) {
        static unsigned tested;
        if (!tested) {
            if (!riplix_math_selftest()) fail("native scalar math selftest", 0);
            serial("NATIVE_MATH_OK sse2 libm\n"); tested = 1;
        }
        return (uintptr_t)rcc_native_ext___rcc_host_math;
    }
#endif
    BIND(rcc_native_load, 2); BIND(rcc_native_store, 3); BIND(rcc_native_gep, 2);
    BIND(rcc_native_stack_mark, 0); BIND(rcc_native_stack_alloc, 1); BIND(rcc_native_stack_restore, 1);
    BIND(rcc_native_function, 2); BIND(rcc_native_image_token, 1); BIND(rcc_native_register, 2);
    BIND(rcc_native_process_frame, 1); BIND(rcc_native_process_slot, 1);
    BIND(rcc_native_process_pc, 0); BIND(rcc_native_process_checkpoint, 0);
    BIND(rcc_native_process_save, 2); BIND(rcc_native_process_set_pc, 1);
    BIND(rcc_native_process_arg, 1); BIND(rcc_native_process_argument, 2);
    BIND(rcc_native_process_call, 3); BIND(rcc_native_process_return, 1);
    BIND(rcc_native_process_request, 4); BIND(rcc_native_process_bad_pc, 0);
    BIND(rcc_native_ext_memset, 3); BIND(rcc_native_ext_memcpy, 3); BIND(rcc_native_ext_memmove, 3);
    BIND(rcc_native_ext___rcc_cell_copy, 3); BIND(rcc_native_ext___rcc_cell_fill, 3);
    BIND(rcc_native_ext_strlen, 1); BIND(rcc_native_ext_strcmp, 2); BIND(rcc_native_ext_strncmp, 3);
    BIND(rcc_native_ext_strcpy, 2); BIND(rcc_native_ext_strncpy, 3); BIND(rcc_native_ext_strcat, 2);
    BIND(rcc_native_ext_strchr, 2); BIND(rcc_native_ext_strrchr, 2); BIND(rcc_native_ext_memchr, 3);
    BIND(rcc_native_ext_memcmp, 3); BIND(rcc_native_ext_abs, 1);
    BIND(rcc_native_ext_labs, 1); BIND(rcc_native_ext_llabs, 1);
#undef BIND
    return 0;
}
static void image_release(Context *c, Module *m) {
    for (unsigned i = m->first; i < m->first + m->count; ++i)
        if (c->functions[i].owner == m) memset(&c->functions[i], 0, sizeof(c->functions[i]));
    pages_release(m->package, m->size);
    if (m->template) pages_release(m->template, m->global_cells * 2);
    memset(m, 0, sizeof(*m)); ++c->images_released;
}
static void images_collect(Context *c) {
    for (unsigned i = 0; i < IMAGES; ++i) if (images[i].package) {
        unsigned owned = 0;
        for (unsigned p = 0; p < PROCESSES; ++p) if (c->processes[p])
            owned |= c->processes[p]->module == &images[i] || c->processes[p]->staged == &images[i];
        if (!owned) image_release(c, &images[i]);
    }
}
U rcc_native_ext_rcc_hosted_image_cancel(Context *c) {
    Process *p = c->processes[c->current];
    if (p->loading) pages_release(p->loading, p->loading_size);
    p->loading = NULL; p->loading_size = p->loading_done = 0; p->staged = NULL;
    images_collect(c); return 0;
}
U rcc_native_ext_rcc_hosted_image_begin(Context *c, U size) {
    rcc_native_ext_rcc_hosted_image_cancel(c);
    if (size < sizeof(ImageHeader) || size > IMAGE_MAX) return 0;
    Process *p = c->processes[c->current];
    p->loading = (void *)pages_alloc((size + 4095) / 4096);
    if (!p->loading) return 0;
    p->loading_size = size; return 1;
}
U rcc_native_ext_rcc_hosted_image_append(Context *c, U source, U count) {
    Process *p = c->processes[c->current];
    if (!p->loading || count > p->loading_size - p->loading_done) return 0;
    size_t start = span(source, count);
    /* Filesystem octets are held in 16-bit guest cells. Reject non-octets. */
    for (size_t i = 0; i < count; ++i) if (c->cells[start + i] > 255) return 0;
    for (size_t i = 0; i < count; ++i) p->loading[p->loading_done++] = c->cells[start + i];
    return 1;
}
U rcc_native_ext_rcc_hosted_image_dependency(Context *c, U path, U capacity) {
    (void)path; (void)capacity;
    Process *p = c->processes[c->current];
    return !p->loading || p->loading_done != p->loading_size ? UINT32_MAX : 0;
}
U rcc_native_ext_rcc_hosted_image_reuse(Context *c) { (void)c; return 0; }

static int image_index(ImageHeader *h, U pointer, U width, size_t *index) {
    U bank = pointer >> 16, offset = pointer & 65535;
    if (bank < h->global_bank || offset >= BANK || !width || width > 4) return 0;
    U at = (bank - h->global_bank) * BANK + offset;
    if (!at || at >= h->global_cells || width > h->global_cells - at) return 0;
    *index = at; return 1;
}
static int image_prepare(Context *c) {
    Process *p = c->processes[c->current];
    if (!p->loading || p->loading_done != p->loading_size) return 0;
    ImageHeader *h = (void *)p->loading;
    for (unsigned i = 0; i < 8; ++i) if (h->magic[i] != "FUSEX001"[i]) return 0;
    if (h->tag != 0x31455852 || h->total != p->loading_size || h->reserved[0] || h->reserved[1] ||
        !h->code_size || h->code_size > IMAGE_MAX || !h->functions || h->functions > 8191 ||
        h->helpers > 128 || h->relocations > IMAGE_MAX / 16 || h->initializers > IMAGE_MAX / 24 ||
        (h->global_bank != 1 && h->global_bank != 32) || !h->global_cells ||
        h->global_cells > (h->global_bank == 1 ? BANK : 16U * BANK)) return 0;
    U metadata = sizeof(*h) + (U)h->functions * 16 + (U)h->helpers * 72 +
                 (U)h->relocations * 16 + (U)h->initializers * 24;
    if (metadata > h->total || h->code_size != h->total - metadata ||
        h->bindings % 8 || h->bindings > h->code_size || (U)h->helpers * 8 > h->code_size - h->bindings) return 0;
    ImageFunction *functions = (void *)(h + 1);
    ImageHelper *helpers = (void *)(functions + h->functions);
    ImageRelocation *relocations = (void *)(helpers + h->helpers);
    ImageInitializer *initializers = (void *)(relocations + h->relocations);
    uint8_t *code = (void *)(initializers + h->initializers);
    uint8_t seen[8192] = {0};
    unsigned count = 0;
    for (unsigned i = 0; i < h->functions; ++i) {
        ImageFunction *f = &functions[i];
        if (!f->local || f->local >= sizeof(seen) || seen[f->local] || f->arity > ARGUMENTS ||
            f->offset >= h->code_size || f->reserved) return 0;
        seen[f->local] = 1;
        if (f->local >= count) count = f->local + 1;
    }
    if (!h->entry || h->entry >= count || !seen[h->entry]) return 0;
    for (unsigned i = 0; i < h->functions; ++i)
        if (functions[i].local == h->entry && functions[i].arity != 1) return 0;
    for (unsigned i = 0; i < h->helpers; ++i)
        if (helpers[i].reserved || !image_helper(helpers[i].name, helpers[i].arity)) {
            serial("IMAGE_REJECTED_HELPER index="); hex(i); serial(" arity="); hex(helpers[i].arity); serial(" name=");
            for (unsigned j = 0; j < 64 && helpers[i].name[j]; ++j) serial_char(helpers[i].name[j]);
            serial("\n"); return 0;
        }
    for (unsigned i = 0; i < h->relocations; ++i)
        if (relocations[i].offset % 8 || relocations[i].offset > h->code_size ||
            h->code_size - relocations[i].offset < 8 || relocations[i].addend >= h->code_size) return 0;
    size_t index;
    if (h->error && !image_index(h, h->error, 1, &index)) return 0;
    for (unsigned i = 0; i < h->initializers; ++i) {
        ImageInitializer *v = &initializers[i];
        U local = v->value ^ 0x80000000U;
        if (!image_index(h, v->pointer, v->width, &index) || v->reserved || v->function > 1 ||
            (v->function && (v->width != 2 || local >= count || !seen[local]))) return 0;
    }
    Module *m = NULL;
    for (unsigned i = 0; i < IMAGES; ++i) if (!images[i].package) { m = &images[i]; break; }
    if (!m) return -1;
    unsigned first = 1, run = 0;
    for (unsigned i = 1; i < FUNCTIONS; ++i) {
        if (c->functions[i].address) { first = i + 1; run = 0; }
        else if (++run == count) break;
    }
    if (run != count) return -1;
    uint16_t *data = (void *)pages_alloc(((size_t)h->global_cells * 2 + 4095) / 4096);
    if (!data) return -1;
    *m = (Module){p->loading, code, p->loading_size, (size_t)h->global_bank * BANK,
                  h->global_cells, data, first, count, first + h->entry, h->error};
    for (unsigned i = 0; i < h->initializers; ++i) {
        ImageInitializer *v = &initializers[i];
        image_index(h, v->pointer, v->width, &index);
        U value = v->function ? (0x80000000U | (first + (v->value ^ 0x80000000U))) : v->value;
        for (unsigned j = 0; j < v->width; ++j) data[index + j] = value >> (j * 16);
    }
    for (unsigned i = 0; i < h->relocations; ++i)
        *(U *)(code + relocations[i].offset) = (uintptr_t)code + relocations[i].addend;
    for (unsigned i = 0; i < h->helpers; ++i)
        ((U *)(code + h->bindings))[i] = image_helper(helpers[i].name, helpers[i].arity);
    for (unsigned i = 0; i < h->functions; ++i) {
        ImageFunction *f = &functions[i];
        c->functions[first + f->local].address = (uintptr_t)code + f->offset;
        c->functions[first + f->local].arity = f->arity;
        c->functions[first + f->local].owner = m;
    }
    p->loading = NULL; p->loading_size = p->loading_done = 0; p->staged = m;
    if (!c->images_loaded++) {
        serial("INDEPENDENT_IMAGE_LOADED functions="); hex(h->functions); serial("\n");
    }
    return 1;
}
U rcc_native_ext_rcc_hosted_image_prepare(Context *c) {
    /* Preserve the kernel API: allocation failure is ENOMEM; invalid code is
     * reported by poll as ENOEXEC, allowing Bourne's script fallback. */
    return image_prepare(c) >= 0;
}
U rcc_native_ext_rcc_hosted_image_poll(Context *c) {
    Module *m = c->processes[c->current]->staged;
    return m ? 0x80000000U | m->entry : UINT32_MAX;
}
U rcc_native_ext_rcc_hosted_image_discard(Context *c, U entry) {
    Process *p = c->processes[c->current];
    if (p->staged && (0x80000000U | p->staged->entry) == entry) p->staged = NULL;
    images_collect(c); return 0;
}
static void image_clear(Context *c, Module *m) {
    memset(c->cells + m->global_start, 0, m->global_cells * 2);
    dirty(c, m->global_start, m->global_cells);
}
static void image_apply(Context *c, Module *m) {
    memcpy(c->cells + m->global_start, m->template, m->global_cells * 2);
    dirty(c, m->global_start, m->global_cells);
    c->errno_address = m->error;
    if (m->global_start == 32U * BANK) {
        memset(&c->heap, 0, sizeof(c->heap));
        c->heap.blocks[0] = (Block){5U * BANK + 1, 27U * BANK - 1, 1};
        c->heap.blocks[1] = (Block){m->global_start + m->global_cells,
            KERNEL_START - m->global_start - m->global_cells, 1};
    }
}
