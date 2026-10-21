/* Minimal ELF64 static PIE writer for the frozen x64 platform ABI.
 * Native .bss is virtual storage, never a giant C16 heap allocation or file. */
#include "link.h"
#include <string.h>

int ld_kernel_mode;
L32 ld_kernel_end, ld_kernel_readonly_end;
static L32 text_end, ro_start, data_end, rela_start, rela_capacity, dynamic_start;
static L32 got_start, got_count, got_capacity;

int ld_kernel_special(const char *name)
{
    return !strcmp(name, "kernel_end") || !strcmp(name, "kernel_readonly_end");
}

static L32 aligned(L32 value, L32 alignment)
{
    if (!alignment)
        alignment = 1;
    if (value > 128UL * 1024 * 1024 - alignment)
        ld_error("kernel memory exceeds 128 MiB");
    return (value + alignment - 1) & ~(alignment - 1);
}

static L32 place(L32 cursor, unsigned group)
{
    struct ld_object **objects = ld_objects.data;
    size_t i, j;
    for (i = 0; i < ld_objects.count; i++)
        for (j = 1; j < objects[i]->section_count; j++) {
            struct ld_section *s = &objects[i]->sections[j];
            unsigned category;
            if (!(s->flags & 2))
                continue;
            if ((s->flags & 5) == 5 || (s->type == 8 && (s->flags & 4)))
                ld_error("kernel section requests writable/executable or BSS code");
            category = s->type == 8 ? 3 : s->flags & 4 ? 0 : s->flags & 1 ? 2 : 1;
            if (category != group)
                continue;
            cursor = aligned(cursor, s->alignment);
            if (s->size > 128UL * 1024 * 1024 - cursor)
                ld_error("kernel memory exceeds 128 MiB");
            s->address = cursor;
            cursor += s->size;
        }
    return cursor;
}

void ld_kernel_layout(void)
{
    struct ld_object **objects = ld_objects.data;
    size_t i, j;
    L32 count = 0;
    for (i = 0; i < ld_objects.count; i++)
        for (j = 1; j < objects[i]->section_count; j++) {
            struct ld_section *s = &objects[i]->sections[j];
            if (s->type == 4) {
                if (s->size / 24 > LD_LIMIT / 24 - count)
                    ld_error("kernel relocation table exceeds capacity");
                count += s->size / 24;
            }
        }
    text_end = place(0, 0);
    if (!text_end)
        ld_error("kernel has no executable code");
    ro_start = aligned(text_end, 4096);
    rela_start = aligned(place(ro_start, 1), 8);
    rela_capacity = count * 24;
    if (rela_start > LD_LIMIT || rela_capacity > LD_LIMIT - rela_start)
        ld_error("kernel file exceeds 16 MiB");
    ld_kernel_readonly_end = aligned(rela_start + rela_capacity, 4096);
    got_start = aligned(place(ld_kernel_readonly_end, 2), 8);
    got_capacity = count;
    if (got_start > LD_LIMIT || count > (LD_LIMIT - got_start) / 8)
        ld_error("kernel GOT exceeds capacity");
    dynamic_start = got_start + count * 8;
    data_end = dynamic_start + 64;
    if (data_end > LD_LIMIT)
        ld_error("kernel file exceeds 16 MiB");
    ld_code_size = data_end;
    ld_kernel_end = aligned(place(aligned(data_end, 4096), 3), 4096);
    if (ld_kernel_end > 128UL * 1024 * 1024 - 4096)
        ld_error("kernel memory exceeds 128 MiB");
}

L32 ld_kernel_got(L64 target, int absolute)
{
    L32 at;
    if (got_count == got_capacity)
        ld_error("kernel GOT reservation overflow");
    at = got_start + got_count++ * 8;
    if (absolute)
        ld_image_patch(at, target, 8);
    else {
        struct ld_fixup *fix;
        if (target >= ld_kernel_end + 4096UL)
            ld_error("kernel GOT target is outside image memory");
        fix = ld_append(&ld_fixups, sizeof *fix);
        fix->offset = at;
        fix->addend = (L32)target;
    }
    return at;
}

static void program(unsigned char *p, L32 type, L32 flags, L32 offset, L32 address, L32 file,
                    L32 memory, L32 alignment)
{
    ld_patch(p, type, 4);
    ld_patch(p + 4, flags, 4);
    ld_patch(p + 8, offset, 8);
    ld_patch(p + 16, address, 8);
    ld_patch(p + 24, address, 8);
    ld_patch(p + 32, file, 8);
    ld_patch(p + 40, memory, 8);
    ld_patch(p + 48, alignment, 8);
}

void ld_kernel_write(FILE *file)
{
    unsigned char header[4096] = {0};
    struct ld_fixup *fix = ld_fixups.data;
    size_t i;
    if (ld_entry >= text_end)
        ld_error("kernel entry is not executable");
    if (ld_fixups.count > rela_capacity / 24)
        ld_error("kernel relocation reservation overflow");
    for (i = 0; i < ld_fixups.count; i++) {
        L32 at = rela_start + (L32)i * 24;
        ld_image_patch(at, fix[i].offset, 8);
        ld_image_patch(at + 8, 8, 8); /* R_X86_64_RELATIVE; no symbol lookup in EFI. */
        ld_image_patch(at + 16, fix[i].addend, 8);
    }
    ld_image_patch(dynamic_start, 7, 8); /* DT_RELA */
    ld_image_patch(dynamic_start + 8, rela_start, 8);
    ld_image_patch(dynamic_start + 16, 8, 8); /* DT_RELASZ */
    ld_image_patch(dynamic_start + 24, ld_fixups.count * 24, 8);
    ld_image_patch(dynamic_start + 32, 9, 8); /* DT_RELAENT */
    ld_image_patch(dynamic_start + 40, 24, 8);
    /* Final DT_NULL pair and all unused padding were zero initialized. */
    memcpy(header, "\177ELF\2\1\1", 7);
    ld_patch(header + 16, 3, 2); /* ET_DYN */
    ld_patch(header + 18, 62, 2);
    ld_patch(header + 20, 1, 4);
    ld_patch(header + 24, ld_entry, 8);
    ld_patch(header + 32, 64, 8);
    ld_patch(header + 52, 64, 2);
    ld_patch(header + 54, 56, 2);
    ld_patch(header + 56, 4, 2);
    program(header + 64, 1, 5, 4096, 0, text_end, text_end, 4096);
    program(header + 120, 1, 4, 4096 + ro_start, ro_start, ld_kernel_readonly_end - ro_start,
            ld_kernel_readonly_end - ro_start, 4096);
    /* One zeroed tail page keeps valid one-past kernel_end GOT/data pointers
     * inside the loader's checked relocation span. BSS itself stays sparse. */
    program(header + 176, 1, 6, 4096 + ld_kernel_readonly_end, ld_kernel_readonly_end,
            data_end - ld_kernel_readonly_end, ld_kernel_end + 4096 - ld_kernel_readonly_end, 4096);
    program(header + 232, 2, 6, 4096 + dynamic_start, dynamic_start, 64, 64, 8);
    if (fwrite(header, 1, sizeof header, file) != sizeof header)
        ld_error("cannot write kernel ELF headers");
    ld_image_write(file);
}
