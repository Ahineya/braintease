/* Bootstrap COFF loader + ELF payload -> PE32+ EFI, without a host linker.
 * This deliberately accepts one self-contained native loader object, not the
 * general Windows ABI, libraries, imports or arbitrary COMDAT selection. */
#include "link.h"
#include <stdlib.h>
#include <string.h>

#define PE_BASE 0x100000UL

struct pe_section {
    L32 size, raw, reloc, count, flags, rva, file;
};

struct pe_symbol {
    const char *name;
    L32 value;
    int section, valid;
};

struct pe_patch {
    L32 at, width;
};
static struct pe_section sections[98];
static struct pe_symbol *symbols;
static unsigned char *coff;
static L32 coff_size, symbol_count, section_count, payload_rva, payload_size;
static struct ld_vector base_relocations;
static struct ld_vector patches;

static int compare_patch(const void *a, const void *b)
{
    L32 x = ((const struct pe_patch *)a)->at, y = ((const struct pe_patch *)b)->at;
    return x < y ? -1 : x > y;
}

static L32 align(L32 n, L32 a)
{
    return (n + a - 1) & ~(a - 1);
}

static void span(L32 at, L32 n)
{
    if (at > coff_size || n > coff_size - at)
        ld_error("COFF span outside loader object");
}

static L32 integer(L32 at, unsigned n)
{
    span(at, n);
    return (L32)ld_read(coff + at, n);
}

static const char *name(L32 at, L32 strings)
{
    L32 p;
    if (integer(at, 4)) {
        char *s = ld_memory(9);
        memcpy(s, coff + at, 8);
        return s;
    }
    p = integer(at + 4, 4);
    if (p < 4 || p >= integer(strings, 4))
        ld_error("invalid COFF string index");
    for (at = strings + p; at < coff_size; at++)
        if (!coff[at])
            return (const char *)coff + strings + p;
    ld_error("unterminated COFF symbol");
    return NULL;
}

static L32 symbol_rva(L32 n)
{
    struct pe_symbol *s;
    if (n >= symbol_count || !symbols[n].valid)
        ld_error("invalid COFF relocation symbol");
    s = &symbols[n];
    if (s->section == -1)
        return s->value;
    if (s->section > 0 && (unsigned)s->section <= section_count && sections[s->section - 1].rva &&
        s->value <= sections[s->section - 1].size)
        return sections[s->section - 1].rva + s->value;
    if (!s->section && !s->value) {
        if (!strcmp(s->name, "kernel_payload_start"))
            return payload_rva;
        if (!strcmp(s->name, "kernel_payload_end"))
            return payload_rva + payload_size;
    }
    ld_error("unresolved/unsupported EFI loader symbol '%s'", s->name);
    return 0;
}

static L32 length(FILE *f)
{
    long n = 0;
    if (fseek(f, 0, SEEK_END) || (n = ftell(f)) < 0 || (unsigned long)n > LD_LIMIT ||
        fseek(f, 0, SEEK_SET))
        ld_error("cannot size bounded EFI input");
    return (L32)n;
}

static void header(unsigned char *p, const char *name, struct pe_section *s)
{
    memcpy(p, name, strlen(name));
    ld_patch(p + 8, s->size, 4);
    ld_patch(p + 12, s->rva, 4);
    ld_patch(p + 16, s->flags & 0x80 ? 0 : align(s->size, 512), 4);
    ld_patch(p + 20, s->file, 4);
    ld_patch(p + 36, s->flags & 0xe00000e0UL, 4);
}

/* One invocation owns this state. Keeping phase-local scratch in separate
 * functions bounds QBE register-allocation memory during guest self-builds. */
static FILE *payload_input;
static L32 headers, file_at, rva = 4096, kept, bss_size;
static struct pe_section *kernel, *relocs;

static void load_loader(const char *loader)
{
    FILE *in;
    L32 table, strings, i;
    ld_path = loader;
    in = fopen(loader, "rb");
    if (!in)
        ld_error("cannot open COFF loader");
    coff_size = length(in);
    if (coff_size < 20 || coff_size > 1048576UL)
        ld_error("unsupported COFF loader size");
    coff = ld_memory(coff_size);
    if (fread(coff, 1, coff_size, in) != coff_size || fclose(in))
        ld_error("cannot read COFF loader");
    for (i = 0; i < coff_size; i++)
        if (coff[i] > 255)
            ld_error("COFF loader contains non-octets");
    section_count = integer(2, 2);
    table = integer(8, 4);
    symbol_count = integer(12, 4);
    if (integer(0, 2) != 0x8664 || integer(16, 2) || !section_count || section_count > 94 ||
        !symbol_count || symbol_count > 16384 || integer(18, 2))
        ld_error("expected x64 COFF relocatable loader");
    span(20, section_count * 40);
    span(table, symbol_count * 18);
    strings = table + symbol_count * 18;
    span(strings, 4);
    span(strings, integer(strings, 4));
    symbols = ld_memory(symbol_count * sizeof *symbols);
    for (i = 0; i < symbol_count;) {
        L32 at = table + i * 18, aux = integer(at + 17, 1);
        struct pe_symbol *s = &symbols[i];
        if (aux >= symbol_count - i)
            ld_error("invalid COFF auxiliary symbol count");
        s->name = name(at, strings);
        s->value = integer(at + 8, 4);
        s->section = (int)integer(at + 12, 2);
        if (s->section >= 32768)
            s->section -= 65536;
        s->valid = 1;
        i += aux + 1;
    }
}

static void layout_sections(void)
{
    L32 i;
    for (i = 0; i < section_count; i++) {
        L32 at = 20 + i * 40;
        struct pe_section *s = &sections[i];
        s->size = integer(at + 16, 4);
        s->raw = integer(at + 20, 4);
        s->reloc = integer(at + 24, 4);
        s->count = integer(at + 32, 2);
        s->flags = integer(at + 36, 4);
        if (integer(at + 8, 4) || integer(at + 12, 4) || integer(at + 28, 4) ||
            integer(at + 34, 2) || s->count == 65535 || (s->flags & 0x01000000UL))
            ld_error("unsupported COFF section");
        if (!(s->flags & 0xe0000000UL) || !s->size || (s->flags & 0x800))
            continue;
        if ((s->flags & 0xa0000000UL) == 0xa0000000UL)
            ld_error("EFI loader requests writable executable memory");
        if (s->size > LD_LIMIT - rva)
            ld_error("EFI loader memory exceeds capacity");
        if (!(s->flags & 0x80))
            span(s->raw, s->size);
        else if (s->count)
            ld_error("EFI BSS cannot contain relocations");
        span(s->reloc, s->count * 10);
        s->rva = rva;
        rva = align(rva + s->size, 4096);
        kept++;
    }
}

static void open_payload(const char *payload)
{
    L32 i;
    unsigned char buffer[64];
    ld_path = payload;
    payload_input = fopen(payload, "rb");
    if (!payload_input)
        ld_error("cannot open kernel payload");
    payload_size = length(payload_input);
    if (payload_size < 64 || fread(buffer, 1, 64, payload_input) != 64 ||
        memcmp(buffer, "\177ELF\2\1\1", 7) || ld_read(buffer + 16, 2) != 3 ||
        ld_read(buffer + 18, 2) != 62 || fseek(payload_input, 0, SEEK_SET))
        ld_error("EFI payload must be an x64 ELF64 PIE kernel");
    kernel = &sections[section_count];
    kernel->size = payload_size;
    kernel->rva = payload_rva = rva;
    kernel->flags = 0x40000040UL;
    rva = align(rva + payload_size, 4096);
    headers = align(128 + 24 + 240 + (kept + 2) * 40, 512);
    file_at = headers;
    for (i = 0; i <= section_count; i++)
        if (sections[i].rva && !(sections[i].flags & 0x80)) {
            sections[i].file = file_at;
            file_at += align(sections[i].size, 512);
        }
}

static void layout_image(void)
{
    L32 i, j;
    /* One 12-octet relocation block per ADDR64 is simple and bounded. PE
     * permits repeated page blocks; no instruction or register is rewritten. */
    for (i = 0; i < section_count; i++)
        if (sections[i].rva)
            for (j = 0; j < sections[i].count; j++) {
                L32 at = sections[i].reloc + j * 10;
                if (integer(at + 8, 2) == 1) {
                    L32 index = integer(at + 4, 4);
                    if (index >= symbol_count || !symbols[index].valid)
                        ld_error("invalid COFF relocation symbol");
                    if (symbols[index].section == -1)
                        continue;
                    L32 *slot = ld_append(&base_relocations, sizeof *slot);
                    *slot = sections[i].rva + integer(at, 4);
                }
            }
    if (!base_relocations.count)
        ld_error("EFI loader must retain a base relocation anchor");
    relocs = &sections[section_count + 1];
    relocs->size = (L32)base_relocations.count * 12;
    relocs->rva = rva;
    relocs->file = file_at;
    relocs->flags = 0x42000040UL;
    rva = align(rva + relocs->size, 4096);
    file_at += align(relocs->size, 512);
    if (file_at > LD_LIMIT || rva > LD_LIMIT)
        ld_error("EFI image exceeds 16 MiB");
    ld_code_size = file_at;
    ld_image_allocate();
}

static void relocate(struct pe_section *s, L32 at)
{
    L32 offset = integer(at, 4), index = integer(at + 4, 4), type = integer(at + 8, 2);
    unsigned width = type == 1 ? 8 : 4;
    L64 value;
    L32 target;
    struct pe_patch *patch;
    if (offset > s->size || width > s->size - offset)
        ld_error("COFF relocation outside section");
    patch = ld_append(&patches, sizeof *patch);
    patch->at = s->file + offset;
    patch->width = width;
    target = symbol_rva(index);
    value = ld_read(coff + s->raw + offset, width);
    if (type == 1)
        value += (symbols[index].section == -1 ? 0 : PE_BASE) + target;
    else if (type == 3) {
        value += target;
        if (value > 0xffffffffULL)
            ld_error("COFF RVA relocation overflow");
    } else if (type >= 4 && type <= 9) {
        if (symbols[index].section == -1)
            ld_error("EFI PC-relative absolute symbol is not position independent");
        if (value & 0x80000000ULL)
            value |= ~0xffffffffULL;
        value += (L64)target - s->rva - offset - 4 - (type - 4);
        if (value > 2147483647ULL && value < ~2147483647ULL)
            ld_error("COFF relative relocation overflow");
    } else
        ld_error("unsupported COFF relocation %lu", type);
    ld_image_patch(s->file + offset, value, width);
}

static void copy_sections(void)
{
    L32 i, j;
    for (i = 0; i < section_count; i++) {
        struct pe_section *s = &sections[i];
        if (!s->rva)
            continue;
        if (s->flags & 0x80) {
            bss_size += s->size;
            continue;
        }
        ld_image_copy(s->file, coff + s->raw, s->size);
        for (j = 0; j < s->count; j++)
            relocate(s, s->reloc + j * 10);
    }
    if (patches.count > 1)
        qsort(patches.data, patches.count, sizeof(struct pe_patch), compare_patch);
    for (i = 1; i < patches.count; i++) {
        struct pe_patch *p = patches.data;
        if (p[i].at < p[i - 1].at + p[i - 1].width)
            ld_error("overlapping COFF relocations");
    }
}

static void append_payload(void)
{
    L32 i, j;
    unsigned char buffer[4096];
    for (i = 0; i < payload_size;) {
        L32 n = payload_size - i < sizeof buffer ? payload_size - i : sizeof buffer;
        if (fread(buffer, 1, n, payload_input) != n)
            ld_error("cannot read kernel payload");
        for (j = 0; j < n; j++)
            if (buffer[j] > 255)
                ld_error("kernel payload contains non-octets");
        ld_image_copy(kernel->file + i, buffer, n);
        i += n;
    }
    if (fclose(payload_input))
        ld_error("cannot close kernel payload");
    for (i = 0; i < base_relocations.count; i++) {
        L32 target = ((L32 *)base_relocations.data)[i], at = relocs->file + i * 12;
        ld_image_patch(at, target & ~4095UL, 4);
        ld_image_patch(at + 4, 12, 4);
        ld_image_patch(at + 8, 0xa000UL | (target & 4095), 2);
    }
}

static void emit_headers(void)
{
    L32 i, j, entry = 0, reloc_start;
    L32 code_size = 0, data_size = 0, first_code = 0;
    unsigned char *pe;
    for (i = 0; i < symbol_count; i++)
        if (symbols[i].valid && !strcmp(symbols[i].name, "efi_main")) {
            if (entry)
                ld_error("duplicate EFI entry");
            if (symbols[i].section <= 0 || (unsigned)symbols[i].section > section_count ||
                !(sections[symbols[i].section - 1].flags & 0x20000000UL) ||
                symbols[i].value >= sections[symbols[i].section - 1].size)
                ld_error("EFI entry is not executable");
            entry = symbol_rva(i);
        }
    if (!entry)
        ld_error("missing EFI entry");
    pe = ld_memory(headers);
    pe[0] = 'M';
    pe[1] = 'Z';
    ld_patch(pe + 60, 128, 4);
    memcpy(pe + 128, "PE\0\0", 4);
    ld_patch(pe + 132, 0x8664, 2);
    ld_patch(pe + 134, kept + 2, 2);
    ld_patch(pe + 148, 240, 2);
    ld_patch(pe + 150, 0x22, 2);
    reloc_start = 152 + 240;
    for (i = j = 0; i < section_count + 2; i++)
        if (sections[i].rva) {
            char label[9];
            struct pe_section *s = &sections[i];
            if (i == section_count)
                strcpy(label, ".kernel");
            else if (i == section_count + 1)
                strcpy(label, ".reloc");
            else
                sprintf(label, "s%lu", (unsigned long)i);
            header(pe + reloc_start + j++ * 40, label, s);
            if (s->flags & 0x20) {
                code_size += align(s->size, 512);
                if (!first_code)
                    first_code = s->rva;
            } else if (!(s->flags & 0x80))
                data_size += align(s->size, 512);
        }
    ld_patch(pe + 152, 0x20b, 2);
    ld_patch(pe + 156, code_size, 4);
    ld_patch(pe + 160, data_size, 4);
    ld_patch(pe + 164, bss_size, 4);
    ld_patch(pe + 168, entry, 4);
    ld_patch(pe + 172, first_code, 4);
    ld_patch(pe + 176, PE_BASE, 8);
    ld_patch(pe + 184, 4096, 4);
    ld_patch(pe + 188, 512, 4);
    ld_patch(pe + 192, 6, 2);
    ld_patch(pe + 200, 6, 2);
    ld_patch(pe + 208, rva, 4);
    ld_patch(pe + 212, headers, 4);
    ld_patch(pe + 220, 10, 2);
    ld_patch(pe + 222, 0x160, 2);
    ld_patch(pe + 224, 1048576, 8);
    ld_patch(pe + 232, 4096, 8);
    ld_patch(pe + 240, 1048576, 8);
    ld_patch(pe + 248, 4096, 8);
    ld_patch(pe + 260, 16, 4);
    ld_patch(pe + 304, relocs->rva, 4);
    ld_patch(pe + 308, relocs->size, 4);
    ld_image_copy(0, pe, headers);
}

void ld_efi(const char *loader, const char *payload, FILE *output)
{
    load_loader(loader);
    layout_sections();
    open_payload(payload);
    layout_image();
    copy_sections();
    append_payload();
    emit_headers();
    ld_image_write(output);
}
