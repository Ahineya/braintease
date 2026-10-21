/* Startup for the existing x64 C16 continuation runtime, expressed in QBE. */
#include "native.h"
#include <string.h>
#define P(...) fprintf(o->file, __VA_ARGS__)

static unsigned long long read_number(const unsigned char *p, unsigned width)
{
    unsigned long long n = 0;
    unsigned i;
    for (i = 0; i < width; i++)
        n |= (unsigned long long)p[i] << (8 * i);
    return n;
}

static unsigned long address(unsigned long base, unsigned long long offset)
{
    unsigned long long linear = (base >> 16) * 64000ULL + (base & 65535);
    /* Unsigned addition implements signed initializer addends modulo 2^64;
     * validating the final logical range rejects underflow and overflow. */
    linear += offset;
    if (linear >= 256ULL * 64000)
        fail("kernel initializer address is outside logical memory");
    return (unsigned long)((linear / 64000 << 16) | (linear % 64000));
}

static unsigned long binding(struct native_output *o, const char *name, unsigned kind)
{
    struct native_binding *b = o->bindings.data;
    unsigned char *data = o->metadata.data;
    size_t i;
    for (i = 0; i < o->bindings.count; i++)
        if (!strcmp(b[i].name, name) && b[i].kind == kind) {
            unsigned long result = (unsigned long)read_number(data + b[i].offset + 16, 8);
            if (!result)
                fail("unresolved kernel initializer '%s'", name);
            return result;
        }
    fail("unknown kernel initializer '%s'", name);
    return 0;
}

void native_kernel_entry(struct native_output *o)
{
    struct native_binding *b = o->bindings.data;
    unsigned char *data = o->metadata.data;
    struct function *main = native_find_function(o->module, "main");
    unsigned long shared = 1, private = 1, bank = 1, token = 0;
    size_t i, at;
    if (!main || main->parameters.count || main->result->kind != TYINT || main->result->bits != 16)
        fail("kernel requires int main(void)");
    for (i = 0; i < o->bindings.count; i++)
        if (b[i].kind == NM_GLOBAL && !symbol(o->module, b[i].name)->shared) {
            if (b[i].cells > 1023999UL - private)
                fail("kernel private globals exceed 16 banks");
            private += b[i].cells;
        }
    if (private > 64000)
        bank = 32;
    private = 1;
    for (i = 0; i < o->bindings.count; i++) {
        unsigned long value;
        unsigned n;
        if (!(b[i].flags & NM_DEFINED))
            fail("unresolved kernel binding '%s'", b[i].name);
        if (b[i].kind == NM_FUNCTION) {
            if (++token >= 65536)
                fail("kernel function table exceeds 65535 tokens");
            value = 0x80000000UL | token;
        } else {
            int kernel = symbol(o->module, b[i].name)->shared;
            unsigned long *cursor = kernel ? &shared : &private;
            unsigned long limit = kernel ? 63999UL : 1024000UL;
            if (b[i].cells > limit - *cursor)
                fail("kernel %s globals exceed runtime capacity", kernel ? "shared" : "private");
            value = (((kernel ? 240UL : bank) + *cursor / 64000) << 16) | (*cursor % 64000);
            *cursor += b[i].cells;
        }
        for (n = 0; n < 8; n++)
            data[b[i].offset + 16 + n] = (unsigned char)((unsigned long long)value >> (n * 8));
    }
    P("# rcc-kernel shared-cells=%lu private-bank=%lu private-cells=%lu\n", shared - 1, bank,
      private);
    P("export function l $rcc_native_entry(l %%ctx) {\n@entry\n"
      "    call $rcc_native_process_memory(l %%ctx)\n");
    if (shared > 1)
        P("    call $rcc_native_kernel_globals(l %%ctx, l %lu)\n", shared - 1);
    if (bank != 1)
        P("    call $rcc_native_global_memory(l %%ctx, l %lu, l %lu)\n", bank, private);
    for (at = NM_HEADER; at < o->metadata.count;) {
        unsigned char *r = data + at;
        unsigned kind = (unsigned)read_number(r, 4);
        unsigned long size = (unsigned long)read_number(r + 8, 4);
        unsigned long cells = (unsigned long)read_number(r + 12, 4);
        const char *name = (const char *)r + NM_RECORD;
        const char *target = name + read_number(r + 48, 4);
        unsigned long offset = (unsigned long)read_number(r + 24, 8);
        if (kind == NM_INITIALIZER) {
            unsigned long long value = read_number(r + 32, 8);
            unsigned mode = (unsigned)read_number(r + 44, 4);
            unsigned width = (unsigned)read_number(r + 40, 4);
            if (mode == 1)
                value = address(binding(o, target, NM_GLOBAL), value);
            else if (mode == 2)
                value = binding(o, target, NM_FUNCTION);
            P("    call $rcc_native_store(l %%ctx, l %lu, l %llu, l %u)\n",
              address(binding(o, name, NM_GLOBAL), offset), value, width);
        } else if (kind == NM_CELLS) {
            const unsigned char *payload = (const unsigned char *)target + read_number(r + 52, 4);
            unsigned long n;
            for (n = 0; n < cells;) {
                unsigned width = cells - n < 4 ? (unsigned)(cells - n) : 4;
                P("    call $rcc_native_store(l %%ctx, l %lu, l %llu, l %u)\n",
                  address(binding(o, name, NM_GLOBAL), offset + n),
                  read_number(payload + n * 2, width * 2), width);
                n += width;
            }
        }
        at += size;
    }
    for (i = 0; i < o->bindings.count; i++)
        if (b[i].kind == NM_FUNCTION)
            P("    call $rcc_native_register(l %%ctx, l %llu, l $rcc_user_%s)\n",
              (unsigned long long)binding(o, b[i].name, NM_FUNCTION) |
                  ((unsigned long long)b[i].cells << 32),
              b[i].name);
    for (i = 0; i < o->bindings.count; i++)
        if (b[i].kind == NM_GLOBAL && !strcmp(b[i].name, "errno"))
            P("    call $rcc_native_errno(l %%ctx, l %lu)\n", binding(o, "errno", NM_GLOBAL));
    P("    %%result =l call $rcc_native_process_start(l %%ctx, l %lu, l 0, l 0, l 1)\n"
      "    ret %%result\n}\n\n",
      binding(o, "main", NM_FUNCTION));
}
