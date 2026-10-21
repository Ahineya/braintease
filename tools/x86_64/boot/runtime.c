/* Freestanding implementation of the existing private C16 native ABI.
 * The shared kernel selects saved contexts; private RAM uses copy-on-write
 * snapshots and a flat active view. Native host return addresses never survive
 * a process switch. Independent packages bind only explicit runtime helpers. */
#include "kernel.h"
#include "terminal-size.h"
#ifdef PERSISTENT_ROOT
#include "block.h"
#endif

#define BANK 64000U
#define CELLS (256U * BANK)
#define KERNEL_START (240U * BANK)
#define NODES 4096
#define FUNCTIONS 65536
#define FRAMES 256
#define SLOTS 512
#define ARGUMENTS 64
#define PROCESSES 64
#define PAGE_CELLS 1024U
#define PRIVATE_PAGES (KERNEL_START / PAGE_CELLS)
typedef uint64_t U;
typedef struct Module Module;
typedef struct Jump Jump;
typedef struct { size_t start, size; unsigned state; } Block;
typedef struct { Block blocks[NODES]; } Heap;
typedef struct {
    U token, pc, base, mark, result, id;
    U args[ARGUMENTS], slots[SLOTS];
    size_t argc;
    unsigned initialized;
} Frame;
typedef struct { U references; uint16_t cells[PAGE_CELLS]; } RamPage;
typedef struct {
    RamPage *pages[PRIVATE_PAGES];
    Frame frames[FRAMES];
    Heap heap;
    size_t depth, stack;
    U errno_address;
    Module *module, *staged;
    uint8_t *loading;
    size_t loading_size, loading_done;
    Jump *jumps;
} Process;
typedef struct {
    uint16_t cells[CELLS];
    struct { U address, arity; Module *owner; } functions[FUNCTIONS];
    Frame frames[FRAMES];
    size_t depth, stack, argc;
    U arguments[ARGUMENTS], op, a, b, result;
    U syscall, checkpoint;
    Heap heap, kernel, initial_heap;
    Process *processes[PROCESSES];
    unsigned current;
    uint8_t dirty[PRIVATE_PAGES];
    uint16_t global_template[BANK];
    uint16_t *extended_template;
    size_t extended_cells;
    U errno_address;
    U next_frame, next_jump, checkpoint_ticks, timer_checkpoints;
    unsigned checkpoint_count, just_returned, images_loaded, images_released;
    unsigned forks, switches, execs, drops;
    size_t file_offset;
    unsigned file_open, file_writable, yes, no;
    uint16_t desc_bank, desc_offset, device_status;
} Context;
static Context context;
extern U rcc_native_entry(Context *);
#ifdef RIPLIX_BOOT
extern const uint8_t root_payload_start[], root_payload_end[];
#ifndef PERSISTENT_ROOT
static uint8_t ram_root[16 * 1024 * 1024];
#endif
static size_t ram_root_size;
#endif

void *memset(void *dest, int value, size_t count) {
    volatile uint8_t *p = dest;
    while (count--) *p++ = (uint8_t)value;
    return dest;
}
void *memcpy(void *dest, const void *src, size_t count) {
    volatile uint8_t *to = dest;
    const volatile uint8_t *from = src;
    while (count--) *to++ = *from++;
    return dest;
}
void *memmove(void *dest, const void *src, size_t count) {
    if ((uintptr_t)dest <= (uintptr_t)src) return memcpy(dest, src, count);
    volatile uint8_t *to = (uint8_t *)dest + count;
    const volatile uint8_t *from = (const uint8_t *)src + count;
    while (count--) *--to = *--from;
    return dest;
}
static size_t linear(U pointer) {
    U bank = pointer >> 16, offset = pointer & 65535;
    if (bank >= 256 || offset >= BANK) fail("C16 pointer", pointer);
    return (size_t)bank * BANK + (size_t)offset;
}
static U pointer(size_t index) { return ((U)(index / BANK) << 16) | (index % BANK); }
static size_t span(U p, U n) {
    size_t start = linear(p);
    if (n > CELLS - start) fail("C16 memory span", p);
    return start;
}
static void dirty(Context *c, size_t start, size_t count) {
    if (!count || start >= KERNEL_START) return;
    size_t end = count > KERNEL_START - start ? KERNEL_START : start + count;
    for (size_t page = start / PAGE_CELLS; page <= (end - 1) / PAGE_CELLS; ++page) c->dirty[page] = 1;
}
static U mmio_read(Context *c, U address);
static U mmio_write(Context *c, U address, U value);
U rcc_native_load(Context *c, U p, U words) {
    if (!words || words > 4) fail("C16 load width", words);
    size_t at = span(p, words);
    U value = 0;
    for (unsigned i = 0; i < words; ++i)
        value |= (at + i < 32 ? mmio_read(c, at + i) : c->cells[at + i]) << (16 * i);
    return value;
}
U rcc_native_store(Context *c, U p, U value, U words) {
    if (!words || words > 4) fail("C16 store width", words);
    size_t at = span(p, words);
    dirty(c, at, words);
    for (unsigned i = 0; i < words; ++i) {
        if (at + i < 32) mmio_write(c, at + i, (uint16_t)(value >> (16 * i)));
        else c->cells[at + i] = (uint16_t)(value >> (16 * i));
    }
    return 0;
}
U rcc_native_gep(Context *c, U p, U delta) {
    (void)c;
    size_t at = linear(p);
    int64_t offset = (int64_t)delta;
    if (offset < -(int64_t)at || offset >= (int64_t)(CELLS - at)) fail("C16 pointer arithmetic", p);
    return pointer((size_t)((int64_t)at + offset));
}
U rcc_native_stack_mark(Context *c) { return c->stack; }
U rcc_native_stack_alloc(Context *c, U cells) {
    if (cells >= 5U * BANK - c->stack) fail("C16 stack exhausted", cells);
    U result = pointer(c->stack); c->stack += cells; return result;
}
U rcc_native_stack_restore(Context *c, U mark) {
    if (mark < 2U * BANK + 1 || mark > c->stack) fail("C16 stack restore", mark);
    c->stack = mark; return 0;
}
static unsigned token_index(U token) {
    U index = (uint32_t)token ^ 0x80000000U;
    if (!index || index >= FUNCTIONS) fail("native function token", token);
    return (unsigned)index;
}
U rcc_native_register(Context *c, U token, U address) {
    unsigned index = token_index(token);
    if (c->functions[index].address || !address) fail("function registration", token);
    c->functions[index].address = address; c->functions[index].arity = token >> 32; return 0;
}
U rcc_native_function(Context *c, U token, U arity) {
    unsigned index = token_index(token);
    if (!c->functions[index].address || c->functions[index].arity != arity)
        fail("native indirect call signature", token);
    return c->functions[index].address;
}
U rcc_native_poll(Context *c) { (void)c; return 0; }

static Block *empty(Heap *heap) {
    for (unsigned i = 0; i < NODES; ++i) if (!heap->blocks[i].state) return &heap->blocks[i];
    fail("allocator metadata exhausted", NODES);
}
static U allocate(Heap *heap, U count) {
    if (!count || count > CELLS) return 0;
    for (unsigned i = 0; i < NODES; ++i) {
        Block *b = &heap->blocks[i];
        if (b->state != 1) continue;
        if (!(b->start % BANK) && b->size) { ++b->start; --b->size; }
        if (b->size < count) continue;
        size_t at = b->start;
        if (b->size > count) *empty(heap) = (Block){at + count, b->size - count, 1};
        b->state = 2; b->size = count;
        return pointer(at);
    }
    return 0;
}
static Block *allocation(Heap *heap, U p) {
    size_t at = linear(p);
    for (unsigned i = 0; i < NODES; ++i)
        if (heap->blocks[i].state == 2 && heap->blocks[i].start == at) return &heap->blocks[i];
    fail("invalid C16 allocation", p);
}
static void release(Heap *heap, U p) {
    if (!p) return;
    Block *b = allocation(heap, p); b->state = 1;
    for (unsigned i = 0; i < NODES; ++i) {
        Block *other = &heap->blocks[i];
        if (other == b || other->state != 1) continue;
        if (b->start + b->size == other->start) { b->size += other->size; other->state = 0; i = (unsigned)-1; }
        else if (other->start + other->size == b->start) {
            b->start = other->start; b->size += other->size; other->state = 0; i = (unsigned)-1;
        }
    }
}
U rcc_native_global_memory(Context *c, U bank, U cells) {
    if (bank != 32 || !cells || cells > 16U * BANK) fail("global reservation", cells);
    c->extended_cells = cells;
    c->heap.blocks[0] = (Block){5U * BANK + 1, 27U * BANK - 1, 1};
    c->heap.blocks[1] = (Block){32U * BANK + cells, KERNEL_START - 32U * BANK - cells, 1};
    return 0;
}
U rcc_native_kernel_globals(Context *c, U cells) {
    if (allocate(&c->kernel, cells) != pointer(KERNEL_START + 1)) fail("shared kernel reservation", cells);
    return 0;
}
U rcc_native_process_memory(Context *c) { (void)c; return 0; }
U rcc_native_errno(Context *c, U address) { span(address, 1); c->errno_address = address; return 0; }

U rcc_native_ext_putchar(Context *c, U value) {
    console_write((char)value);
    c->yes += value == 'Y'; c->no += value == 'N';
    return value & 65535;
}
U rcc_native_ext_puts(Context *c, U p) {
    size_t at = linear(p);
    for (; at < CELLS; ++at) {
        if (!c->cells[at]) { rcc_native_ext_putchar(c, '\n'); return 0; }
        rcc_native_ext_putchar(c, c->cells[at]);
    }
    fail("unterminated C16 string", p);
}
U rcc_native_ext_malloc(Context *c, U size) { return allocate(&c->heap, size); }
U rcc_native_ext_free(Context *c, U p) { release(&c->heap, p); return 0; }
U rcc_native_ext_calloc(Context *c, U count, U size) {
    if (!count || size > UINT64_MAX / count) return 0;
    U total = count * size, p = allocate(&c->heap, total);
    if (p) { dirty(c, span(p, total), total); memset(&c->cells[linear(p)], 0, (size_t)total * 2); }
    return p;
}
U rcc_native_ext_realloc(Context *c, U old, U size) {
    if (!old) return allocate(&c->heap, size);
    if (!size) { release(&c->heap, old); return 0; }
    U previous = allocation(&c->heap, old)->size;
    U next = allocate(&c->heap, size);
    if (next) {
        U count = previous < size ? previous : size;
        dirty(c, span(next, count), count);
        memmove(&c->cells[span(next, count)], &c->cells[span(old, count)], (size_t)count * 2);
        release(&c->heap, old);
    }
    return next;
}
U rcc_native_ext_memset(Context *c, U dest, U value, U count) {
    size_t at = span(dest, count);
    dirty(c, at, count);
    for (U i = 0; i < count; ++i) c->cells[at + i] = (uint16_t)value;
    return dest;
}
U rcc_native_ext_memmove(Context *c, U dest, U src, U count) {
    dirty(c, span(dest, count), count);
    memmove(&c->cells[span(dest, count)], &c->cells[span(src, count)], (size_t)count * 2);
    return dest;
}
U rcc_native_ext_memcpy(Context *c, U dest, U src, U count) { return rcc_native_ext_memmove(c, dest, src, count); }
U rcc_native_ext___rcc_cell_copy(Context *c, U dest, U src, U count) { return rcc_native_ext_memmove(c, dest, src, count); }
U rcc_native_ext___rcc_cell_fill(Context *c, U dest, U value, U count) { return rcc_native_ext_memset(c, dest, value, count); }
U rcc_native_ext_abs(Context *c, U value) { (void)c; return (uint16_t)((int16_t)value < 0 ? -(int16_t)value : value); }
U rcc_native_ext_labs(Context *c, U value) { (void)c; return (uint32_t)((int32_t)value < 0 ? 0U - (uint32_t)value : value); }
U rcc_native_ext_llabs(Context *c, U value) { (void)c; return value >> 63 ? 0 - value : value; }
U rcc_native_ext_strlen(Context *c, U text) {
    size_t at = linear(text), end = at;
    while (end < CELLS && c->cells[end]) ++end;
    if (end == CELLS) fail("unterminated C16 strlen", text);
    return end - at;
}
U rcc_native_ext_strncmp(Context *c, U a, U b, U count) {
    size_t left = linear(a), right = linear(b);
    for (U i = 0; i < count; ++i) {
        if (i >= CELLS - left || i >= CELLS - right) fail("string comparison bounds", a);
        uint16_t x = c->cells[left + i], y = c->cells[right + i];
        if (x != y) return (uint16_t)(x < y ? -1 : 1);
        if (!x) return 0;
    }
    return 0;
}
U rcc_native_ext_strcmp(Context *c, U a, U b) { return rcc_native_ext_strncmp(c, a, b, CELLS); }
U rcc_native_ext_memcmp(Context *c, U a, U b, U n) {
    size_t left = span(a, n), right = span(b, n);
    for (U i = 0; i < n; ++i)
        if (c->cells[left + i] != c->cells[right + i]) return (uint16_t)(c->cells[left + i] < c->cells[right + i] ? -1 : 1);
    return 0;
}
U rcc_native_ext_strcpy(Context *c, U dest, U src) {
    return rcc_native_ext_memcpy(c, dest, src, rcc_native_ext_strlen(c, src) + 1);
}
U rcc_native_ext_strncpy(Context *c, U dest, U src, U n) {
    size_t d = span(dest, n), s = linear(src);
    dirty(c, d, n);
    U i = 0;
    for (; i < n; ++i) {
        if (s + i >= CELLS) fail("strncpy source", src);
        if (!c->cells[s + i]) break;
        c->cells[d + i] = c->cells[s + i];
    }
    for (; i < n; ++i) c->cells[d + i] = 0;
    return dest;
}
U rcc_native_ext_strcat(Context *c, U dest, U src) {
    rcc_native_ext_strcpy(c, rcc_native_gep(c, dest, rcc_native_ext_strlen(c, dest)), src); return dest;
}
U rcc_native_ext_strchr(Context *c, U p, U value) {
    for (size_t at = linear(p); at < CELLS; ++at) {
        if (c->cells[at] == (uint16_t)value) return pointer(at);
        if (!c->cells[at]) return 0;
    }
    fail("unterminated strchr", p);
}
U rcc_native_ext_strrchr(Context *c, U p, U value) {
    U result = 0;
    for (size_t at = linear(p); at < CELLS; ++at) {
        if (c->cells[at] == (uint16_t)value) result = pointer(at);
        if (!c->cells[at]) return result;
    }
    fail("unterminated strrchr", p);
}
U rcc_native_ext_memchr(Context *c, U p, U value, U n) {
    size_t at = span(p, n);
    for (U i = 0; i < n; ++i) if (c->cells[at + i] == (uint16_t)value) return pointer(at + i);
    return 0;
}

static Frame *frame(Context *c) {
    if (!c->depth) fail("continuation without frame", 0);
    return &c->frames[c->depth - 1];
}
U rcc_native_process_frame(Context *c, U cells) {
    Frame *f = frame(c);
    if (!f->initialized) {
        f->id = ++c->next_frame;
        f->mark = c->stack; f->base = rcc_native_stack_alloc(c, cells); f->initialized = 1;
    }
    return f->base;
}
U rcc_native_process_pc(Context *c) { return frame(c)->pc; }
U rcc_native_process_set_pc(Context *c, U pc) {
    Frame *f = frame(c); f->pc = pc; memset(f->slots, 0, sizeof(f->slots)); return 0;
}
U rcc_native_process_save(Context *c, U slot, U value) {
    if (slot >= SLOTS) fail("continuation slot capacity", slot);
    frame(c)->slots[slot] = value; return 0;
}
U rcc_native_process_slot(Context *c, U slot) {
    if (slot >= SLOTS) fail("continuation slot capacity", slot);
    return frame(c)->slots[slot];
}
U rcc_native_process_arg(Context *c, U index) {
    Frame *f = frame(c);
    if (index >= f->argc) fail("continuation argument", index);
    return f->args[index];
}
U rcc_native_process_argument(Context *c, U index, U value) {
    if (!index) c->argc = 0;
    if (index != c->argc || index >= ARGUMENTS) fail("continuation argument order", index);
    c->arguments[c->argc++] = value; return 0;
}
U rcc_native_process_call(Context *c, U token, U count, U result) {
    if (c->depth == FRAMES || count > ARGUMENTS || (count && count != c->argc))
        fail("continuation call capacity", count);
    rcc_native_function(c, token, count);
    Frame *f = &c->frames[c->depth++]; memset(f, 0, sizeof(*f));
    f->token = token; f->result = result; f->argc = count;
    memcpy(f->args, c->arguments, (size_t)count * sizeof(U));
    c->argc = 0; return 0;
}
static void result_slot(Context *c, U slot, U value) {
    if (slot > SLOTS) fail("continuation result slot", slot);
    if (slot) frame(c)->slots[slot - 1] = value;
}
static void jumps_prune(Context *c);
U rcc_native_process_return(Context *c, U value) {
    Frame *f = frame(c);
    U result = f->result; c->stack = f->mark; --c->depth;
    c->just_returned = 1;
    jumps_prune(c);
    if (c->depth) result_slot(c, result, value);
    else { c->op = 4; c->a = value; }
    return 0;
}
U rcc_native_process_request(Context *c, U op, U a, U b, U result) {
    c->op = op; c->a = a; c->b = b; c->result = result; return 0;
}
U rcc_native_process_checkpoint(Context *c) {
    if (!c->checkpoint || c->just_returned || ++c->checkpoint_count < 256) return 0;
    c->checkpoint_count = 0;
    for (size_t i = 0; i < c->depth; ++i)
        if (c->frames[i].token == c->checkpoint || c->frames[i].token == c->syscall) return 0;
    U now = timer_ticks();
    if (now == c->checkpoint_ticks) return 0;
    c->checkpoint_ticks = now; ++c->timer_checkpoints; return 1;
}
U rcc_native_process_bad_pc(Context *c) { (void)c; fail("continuation program counter", 0); }
U rcc_native_ext_rcc_hosted_bind_syscall(Context *c, U token) { rcc_native_function(c, token, 1); c->syscall = token; return 0; }
U rcc_native_ext_rcc_hosted_bind_checkpoint(Context *c, U token) { rcc_native_function(c, token, 0); c->checkpoint = token; return 0; }
U rcc_native_ext_rcc_hosted_stopping(Context *c) { (void)c; return 0; }
static void pages_release(void *address, size_t bytes) {
    for (size_t i = 0; i < (bytes + 4095) / 4096; ++i)
        page_free((uintptr_t)address + i * 4096);
}
#include "images.inc"
#include "jumps.inc"
#include "process.inc"
U rcc_native_process_start(Context *c, U token, U argc, U argv, U hosted) {
    (void)argc; (void)argv;
    if (!hosted) fail("standalone process scheduler is unsupported", 0);
    U initial_pages = page_available() + page_mapping_owned();
#ifdef BOOT_PAGE_OWNERSHIP_TEST
#include "../tests/page-ownership.inc"
#endif
    c->processes[0] = process_allocate();
    if (!c->processes[0]) fail("initial kernel context allocation", sizeof(Process));
    memcpy(c->global_template, c->cells + BANK, sizeof(c->global_template));
    if (c->extended_cells) {
        c->extended_template = (void *)pages_alloc((c->extended_cells * 2 + 4095) / 4096);
        if (!c->extended_template) fail("private global template allocation", c->extended_cells);
        memcpy(c->extended_template, c->cells + 32U * BANK, c->extended_cells * 2);
    }
    memcpy(&c->initial_heap, &c->heap, sizeof(Heap));
    rcc_native_process_call(c, token, 0, 0);
    for (;;) {
        Frame *f = frame(c);
        unsigned index = token_index(f->token);
        U (*step)(Context *) = (void *)c->functions[index].address;
        c->just_returned = 0;
        step(c);
        U op = c->op; c->op = 0;
        switch (op) {
        case 0: break;
        case 4: {
            U status = c->a;
            for (unsigned i = 0; i < PROCESSES; ++i)
                if (c->processes[i]) { process_release(c->processes[i]); c->processes[i] = NULL; }
            images_collect(c);
            if (c->extended_template) pages_release(c->extended_template, c->extended_cells * 2);
            U balance = page_available() + page_mapping_owned();
            if (balance != initial_pages) fail("context physical page leak", initial_pages - balance);
            serial("CONTEXT_PAGES_OK mapping_pages="); hex(page_mapping_owned()); serial("\n");
            return status;
        }
        case 6: result_slot(c, c->result, allocate(&c->kernel, c->a)); break;
        case 7: release(&c->kernel, c->a); result_slot(c, c->result, 0); break;
        case 8: process_clone(c, c->a, c->result); break;
        case 9: case 10: process_switch(c, c->a, c->result, op == 10); break;
        case 11: process_exec(c, c->a, c->b); break;
        case 12:
            rcc_native_process_argument(c, 0, c->a);
            rcc_native_process_call(c, c->syscall, 1, c->result); break;
        case 13: case 14: process_jump(c, op, c->a, c->b, c->result); break;
        case 15:
            if (c->checkpoint) rcc_native_process_call(c, c->checkpoint, 0, 0);
            break;
        default: fail("unsupported process operation", op);
        }
        if (c->depth && c->functions[token_index(frame(c)->token)].owner && rcc_native_process_checkpoint(c))
            rcc_native_process_call(c, c->checkpoint, 0, 0);
    }
}

static U nanoseconds(void) {
    U ticks = rdtsc() - boot.tsc_start;
    return (ticks / boot.tsc_hz) * 1000000000 + (ticks % boot.tsc_hz) * 1000000000 / boot.tsc_hz;
}
#ifdef BOOT_NETWORK
#include "network.inc"
#endif
#include "entropy.inc"
#include "acpi.h"
static unsigned reboot_requested;
#ifdef BOOT_WIFI_LIVE
#include "wifi-control.inc"
#endif
static void device(Context *c, U command) {
    if(command==11) {
        U address=((U)c->desc_bank<<16)|c->desc_offset;
        size_t at;
        if(!entropy_span(address,4,&at)){c->device_status=4;return;}
        uint16_t reply[4];
        memcpy(reply,c->cells+at,sizeof(reply));
        unsigned rows=0,columns=0;
        (void)console_dimensions(&rows,&columns);
        c->device_status=x64_terminal_size_reply(reply,4,rows,columns);
        if(!c->device_status){dirty(c,at,4);memcpy(c->cells+at,reply,sizeof(reply));}
        return;
    }
    if(command==0x105) {
        U p=((U)c->desc_bank<<16)|c->desc_offset;size_t at;
        if(!entropy_span(p,4,&at)){c->device_status=4;return;}
        uint16_t *d=c->cells+at;
        if(d[0]!=1 || d[1]!=4 || d[2]>2 || d[3]){c->device_status=5;return;}
        dirty(c,at,4);d[3]=acpi.reset.valid?1:0;c->device_status=0;
        if(d[2]==1 && acpi.reset.valid)reboot_requested=1;
        if(d[2]==2)reboot_requested=0;
        return;
    }
    if(command==12){entropy_device(c);return;}
#ifdef BOOT_WIFI_LIVE
    if(command==WIFI_CONTROL_COMMAND){wifi_control_device(c);return;}
#endif
#ifdef BOOT_NETWORK
    if(command>=0x100 && command<=0x103) {network_device(c,command);return;}
#endif
    U address = ((U)c->desc_bank << 16) | c->desc_offset;
    size_t at = span(address, 2);
    uint16_t *d = &c->cells[at];
    unsigned count = d[1];
    if (count < 2 || count > 4096) { c->device_status = 3; return; }
    span(address, count);
    dirty(c, at, count);
    c->device_status = 7;
    if (d[0] != 1) { c->device_status = 2; return; }
    if (command == 2 && count == 4) { d[2] = 0x7002 | (entropy_available()?0x8000:0); d[3] = 0; c->device_status = 0; }
#ifdef BOOT_NETWORK
    if (command == 2 && count == 4) d[2] |= 0x20;
#endif
    if (command == 4 && count == 8) {
        for (unsigned i = 2; i < 8; ++i) d[i] = 0;
        d[6] = 60; c->device_status = 0;
    }
    if ((command == 8 || command == 9) && count == 10) {
        U ns = nanoseconds(), seconds = ns / 1000000000;
        if (command == 9) seconds += boot.epoch;
        for (unsigned i = 0; i < 4; ++i) d[2 + i] = (uint16_t)(seconds >> (i * 16));
        U nanos = ns % 1000000000;
        d[6] = nanos; d[7] = nanos >> 16; d[8] = 0x4240; d[9] = 0x000f;
        c->device_status = 0;
    }
    if (command == 10 && count == 4 && d[2] <= 10) {
        timer_idle(d[2]);
        c->device_status = 0;
    }
    if (command == 14 && count == 8 && d[4] <= 4096) {
        size_t src = span(((U)d[2] << 16) | d[3], d[4]);
        for (unsigned i = 0; i < d[4]; ++i) rcc_native_ext_putchar(c, c->cells[src + i]);
        d[6] = d[4]; c->device_status = 0;
    }
}
static U mmio_read(Context *c, U address) {
    if (address >= 32) fail("unsupported MMIO register", address);
    switch (address) {
    case 1: return 1;
    case 3: return 0;
    case 4: return (uint16_t)(rdtsc() >> 8);
    case 30: return c->device_status;
    case 31: return 1;
    default: return c->cells[address];
    }
}
static U mmio_write(Context *c, U address, U value) {
    if (address >= 32) fail("unsupported MMIO register", address);
    if (!address) return rcc_native_ext_putchar(c, value);
    if (address == 27) c->desc_bank = value;
    else if (address == 28) c->desc_offset = value;
    else if (address == 29) device(c, value);
    else { dirty(c, address, 1); c->cells[address] = value; }
    return 0;
}
U rcc_native_ext_mmio_read(Context *c, U address) { return mmio_read(c, address); }
U rcc_native_ext_mmio_write(Context *c, U address, U value) { return mmio_write(c, address, value); }
U rcc_native_ext_rcc_hosted_idle(Context *c, U ms) {
    (void)c;
    if (ms > 10) ms = 10;
    timer_idle(ms);
    return 0;
}

U rcc_native_ext_rcc_hosted_gx(Context *c, U operation, U arguments) {
    (void)c; (void)arguments;
    /* No hosted window service: its shortcut queue is empty; all other
     * operations explicitly return EOPNOTSUPP. */
    return operation == 11 ? 0 : (uint32_t)-95;
}
U rcc_native_ext_rcc_hosted_input(Context *c) { (void)c; return (uint16_t)console_input(); }
#ifdef RIPLIX_BOOT
#include "root.inc"
#endif

_Noreturn void unsupported_service(const char *name) { fail(name, 0); }
#ifdef BOOT_WIFI_BRINGUP
#include "wifi.h"
#endif
void runtime_run(void) {
    Context *c = &context;
#ifdef BOOT_WIFI_PROBE
    wifi_probe();
#endif
#ifdef BOOT_WIFI_BRINGUP
#ifdef BOOT_WIFI_USERLAND
    extern void wifi_runtime_init(void);
    wifi_runtime_init();
#else
    wifi_bringup();
#endif
#endif
#ifdef BOOT_NETWORK
    nic_init();
#endif
#ifdef RIPLIX_BOOT
    root_init();
    input_init();
#endif
    c->stack = 2U * BANK + 1;
    c->heap.blocks[0] = (Block){5U * BANK + 1, KERNEL_START - 5U * BANK - 1, 1};
    c->kernel.blocks[0] = (Block){KERNEL_START + 1, CELLS - KERNEL_START - 1, 1};
    U status = rcc_native_entry(c);
    if (status) fail("RCC entry status", status);
#ifdef BOOT_NETWORK
    nic_stop();
#endif
    serial("TIMER_RUNTIME_OK ticks="); hex(timer_ticks());
    serial(" checkpoints="); hex(c->timer_checkpoints); serial("\n");
#ifdef RIPLIX_BOOT
#ifdef RIPLIX_BOOTSTRAP
    serial("RIPLIX_KERNEL_BOOTSTRAP_OK\n");
    screen_status("Riplix kernel mounted its RAM root; PID 1 and file syscalls passed");
#else
    serial("RIPLIX_USERSPACE_SESSION_OK forks="); hex(c->forks);
    serial(" switches="); hex(c->switches); serial(" execs="); hex(c->execs);
    serial(" drops="); hex(c->drops); serial("\n");
    if (c->images_loaded != c->images_released) fail("application image leak", c->images_loaded - c->images_released);
    serial("INDEPENDENT_IMAGES_OK loaded="); hex(c->images_loaded); serial(" released="); hex(c->images_released); serial("\n");
#ifdef PERSISTENT_ROOT
    if (block_flush()) fail("root disk final flush", 0);
    block_report();
    screen_status("Riplix session stopped; disk filesystem synced");
#else
    screen_status("Riplix session stopped; RAM filesystem synced");
#endif
#endif
#elif defined(PROCESS_TEST)
    if (c->forks != 4 || c->switches != 16 || c->execs != 1 || c->drops != 4)
        fail("process operation acceptance counters", c->forks);
    serial("RCC_PROCESS_DRIVER_OK\n");
    screen_status("Native process checks passed: private RAM, shared kernel, fork, exec and reuse");
#else
    if (c->yes != 8 || c->no) fail("RCC runtime acceptance", c->yes);
    serial("RCC_FREESTANDING_RUNTIME_OK\n");
    screen_status("RCC-generated x86_64 code passed all C16 runtime checks");
#endif
    if(reboot_requested) {
        serial("PLATFORM_REBOOT after clean guest exit and device/disk flush\n");
        console_boot_status(0,"Restarting machine",0);
        out8(acpi.reset.port,acpi.reset.value);
        uint64_t end=rdtsc()+boot.tsc_hz;
        while(rdtsc()<end)__asm__ volatile("pause");
        console_boot_status(2,"Firmware reset returned","machine remains stopped");
    }
}
