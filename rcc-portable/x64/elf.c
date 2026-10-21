/* ELF64 relocatable objects, explicitly encoded as little-endian octets.
 * No host structs, ELF headers, host pointers or endianness enter the wire. */
#include "x64.h"
#include <string.h>

enum { NSH = 13, SYMTAB = 9, STRTAB = 10, SHSTRTAB = 11 };

static U32 string(struct buffer *b, const char *s)
{
    U32 at = (U32)b->size;
    do {
        x64_byte(b, (unsigned char)*s);
    } while (*s++);
    return at;
}

static void patch(struct relocation *r, U64 value, unsigned width)
{
    unsigned i;
    struct buffer *b = &x64_sections[r->section].bytes;
    if (r->offset > b->size || width > b->size - r->offset)
        x64_error("relocation outside section");
    for (i = 0; i < width; i++) {
        b->data[r->offset + i] = (unsigned char)(value & 255);
        value >>= 8;
    }
}

static void symbol_record(struct buffer *b, struct symbol *s)
{
    x64_number(b, s->name_at, 4);
    x64_byte(b, ((s->global || !s->defined) ? 16 : 0) | s->type);
    x64_byte(b, 0);
    x64_number(b, s->defined ? s->section + 1 : 0, 2);
    x64_number(b, s->value, 8);
    x64_number(b, s->size, 8);
}

void x64_write_object(FILE *output)
{
    struct buffer data[NSH], wire = {0};
    U32 offsets[NSH], names[NSH], sizes[NSH], alignment[NSH];
    U32 first_global = 1, index = 1, shoff;
    size_t i, j;
    struct symbol *s = x64_symbols.data;
    const char *labels[NSH] = {
        "",           ".text",      ".data",          ".bss",         ".rodata",
        ".rela.text", ".rela.data", ".rela.bss",      ".rela.rodata", ".symtab",
        ".strtab",    ".shstrtab",  ".note.GNU-stack"};
    memset(data, 0, sizeof data);
    memset(offsets, 0, sizeof offsets);
    memset(sizes, 0, sizeof sizes);
    for (i = 0; i < NSH; i++)
        alignment[i] = 1;
    x64_byte(&data[STRTAB], 0);
    x64_byte(&data[SHSTRTAB], 0);
    for (i = 0; i < NSH; i++)
        names[i] = string(&data[SHSTRTAB], labels[i]);
    for (i = 0; i < x64_symbols.count; i++) {
        if (s[i].local && !s[i].defined)
            x64_error("undefined local symbol '%s'", s[i].name);
        s[i].name_at = string(&data[STRTAB], s[i].name);
        if (s[i].defined && !s[i].global)
            s[i].index = index++;
    }
    first_global = index;
    for (i = 0; i < x64_symbols.count; i++)
        if (!s[i].defined || s[i].global)
            s[i].index = index++;
    for (i = 0; i < 24; i++)
        x64_byte(&data[SYMTAB], 0);
    /* The two stable passes match the index assignment, without an O(n^2)
     * search through every compiler-generated local label. */
    for (j = 0; j < x64_symbols.count; j++)
        if (s[j].defined && !s[j].global)
            symbol_record(&data[SYMTAB], &s[j]);
    for (j = 0; j < x64_symbols.count; j++)
        if (!s[j].defined || s[j].global)
            symbol_record(&data[SYMTAB], &s[j]);
    for (i = 0; i < x64_relocations.count; i++) {
        struct relocation *r = &((struct relocation *)x64_relocations.data)[i];
        struct symbol *target = &s[r->value.symbol];
        if ((r->type == 2 || r->type == 4) && target->defined && !target->global &&
            target->section == r->section) {
            U64 value = (U64)target->value + r->value.addend - r->offset;
            if (value > 2147483647ULL && value < ~(U64)2147483647)
                x64_error("branch displacement overflow");
            patch(r, value, 4);
        } else {
            struct buffer *b = &data[5 + r->section];
            x64_number(b, r->offset, 8);
            x64_number(b, ((U64)target->index << 32) | r->type, 8);
            x64_number(b, r->value.addend, 8);
        }
    }
    for (i = 0; i < NSECTIONS; i++) {
        data[i + 1] = x64_sections[i].bytes;
        sizes[i + 1] = x64_sections[i].size;
        alignment[i + 1] = x64_sections[i].alignment;
    }
    for (i = 5; i <= 9; i++)
        alignment[i] = 8;
    for (i = 0; i < 64; i++)
        x64_byte(&wire, 0);
    for (i = 1; i < NSH; i++) {
        while (wire.size % alignment[i])
            x64_byte(&wire, 0);
        offsets[i] = (U32)wire.size;
        if (i != BSS + 1) {
            for (j = 0; j < data[i].size; j++)
                x64_byte(&wire, data[i].data[j]);
            sizes[i] = (U32)data[i].size;
        }
    }
    while (wire.size % 8)
        x64_byte(&wire, 0);
    shoff = (U32)wire.size;
    for (i = 0; i < NSH; i++) {
        unsigned type = i == 0               ? 0
                        : i == 3             ? 8
                        : i >= 5 && i <= 8   ? 4
                        : i == 9             ? 2
                        : i == 10 || i == 11 ? 3
                                             : 1;
        unsigned flags = i == 1 ? 6 : i == 2 || i == 3 ? 3 : i == 4 ? 2 : 0;
        x64_number(&wire, i ? names[i] : 0, 4);
        x64_number(&wire, type, 4);
        x64_number(&wire, flags, 8);
        x64_number(&wire, 0, 8);
        x64_number(&wire, i ? offsets[i] : 0, 8);
        x64_number(&wire, i ? sizes[i] : 0, 8);
        x64_number(&wire, i >= 5 && i <= 8 ? SYMTAB : i == SYMTAB ? STRTAB : 0, 4);
        x64_number(&wire, i >= 5 && i <= 8 ? i - 4 : i == SYMTAB ? first_global : 0, 4);
        x64_number(&wire, i ? alignment[i] : 0, 8);
        x64_number(&wire, i >= 5 && i <= 9 ? 24 : 0, 8);
    }
    /* Build the header separately and copy exactly 64 octets. */
    {
        struct buffer h = {0};
        x64_byte(&h, 127);
        x64_byte(&h, 'E');
        x64_byte(&h, 'L');
        x64_byte(&h, 'F');
        x64_byte(&h, 2);
        x64_byte(&h, 1);
        x64_byte(&h, 1);
        while (h.size < 16)
            x64_byte(&h, 0);
        x64_number(&h, 1, 2);
        x64_number(&h, 62, 2);
        x64_number(&h, 1, 4);
        x64_number(&h, 0, 8);
        x64_number(&h, 0, 8);
        x64_number(&h, shoff, 8);
        x64_number(&h, 0, 4);
        x64_number(&h, 64, 2);
        x64_number(&h, 0, 2);
        x64_number(&h, 0, 2);
        x64_number(&h, 64, 2);
        x64_number(&h, NSH, 2);
        x64_number(&h, SHSTRTAB, 2);
        for (i = 0; i < 64; i++)
            wire.data[i] = h.data[i];
    }
    for (i = 0; i < wire.size; i++)
        if (putc(wire.data[i] & 255, output) == EOF)
            x64_error("cannot write ELF object");
}
