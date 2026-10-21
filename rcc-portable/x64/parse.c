#include "x64.h"
#include <ctype.h>
#include <stdlib.h>
#include <string.h>

static unsigned block_comment;

/* symbol_id can reallocate the vector; obtain its data pointer afterwards. */
static struct symbol *lookup(const char *name)
{
    long id = x64_symbol_id(name);
    return &((struct symbol *)x64_symbols.data)[id];
}

static unsigned split(char *text, char **parts, unsigned capacity)
{
    unsigned n = 0, depth = 0, quote = 0, escape = 0;
    char *s;
    if (!*x64_trim(text))
        return 0;
    parts[n++] = x64_trim(text);
    for (s = text; *s; s++) {
        if (quote) {
            if (!escape && *s == '"')
                quote = 0;
            escape = !escape && *s == '\\';
            continue;
        }
        if (*s == '"') {
            quote = 1;
            continue;
        }
        if (*s == '(')
            depth++;
        else if (*s == ')') {
            if (!depth)
                x64_error("unmatched closing parenthesis");
            depth--;
        } else if (*s == ',' && !depth) {
            *s = 0;
            if (n == capacity)
                x64_error("too many operands");
            parts[n++] = x64_trim(s + 1);
        }
    }
    if (quote || depth)
        x64_error("unterminated string or address");
    for (depth = 0; depth < n; depth++)
        parts[depth] = x64_trim(parts[depth]);
    return n;
}

static U64 constant(char *s)
{
    struct x64_expression e = x64_expression(s);
    if (e.symbol >= 0)
        x64_error("constant expression required");
    return e.addend;
}

static void align_to(U64 alignment, unsigned fill, U64 maximum)
{
    U32 padding;
    if (x64_current_section >= NSECTIONS)
        x64_error("no allocatable section selected");
    if (!alignment || alignment > 4096 || (alignment & (alignment - 1)))
        x64_error("alignment must be a power of two up to 4096");
    if (x64_sections[x64_current_section].alignment < alignment)
        x64_sections[x64_current_section].alignment = (U32)alignment;
    padding = (U32)(0 - x64_sections[x64_current_section].size) & ((U32)alignment - 1);
    if (maximum && padding > maximum)
        return;
    while (padding--)
        x64_emit(fill);
}

static void string_data(char *text, unsigned terminate)
{
    char *s = x64_trim(text);
    unsigned c, n;
    if (*s++ != '"')
        x64_error("string directive requires a quoted string");
    while (*s && *s != '"') {
        c = (unsigned char)*s++;
        if (c == '\\') {
            c = (unsigned char)*s++;
            if (c >= '0' && c <= '7') {
                c -= '0';
                n = 1;
                while (n < 3 && *s >= '0' && *s <= '7') {
                    c = c * 8 + *s++ - '0';
                    n++;
                }
            } else if (c == 'x') {
                c = 0;
                n = 0;
                while (isxdigit((unsigned char)*s)) {
                    c = c * 16 + (isdigit((unsigned char)*s)
                                      ? *s - '0'
                                      : tolower((unsigned char)*s) - 'a' + 10);
                    s++;
                    n++;
                    if (c > 255)
                        x64_error("string escape exceeds one octet");
                }
                if (!n)
                    x64_error("hex escape needs digits");
            } else if (c == 'n')
                c = '\n';
            else if (c == 'r')
                c = '\r';
            else if (c == 't')
                c = '\t';
            else if (c == 'b')
                c = '\b';
            else if (c == 'f')
                c = '\f';
            else if (c != '\\' && c != '"')
                x64_error("unsupported string escape");
        }
        if (c > 255)
            x64_error("string character exceeds one octet");
        x64_emit(c);
    }
    if (*s++ != '"' || *x64_trim(s))
        x64_error("invalid string directive");
    if (terminate)
        x64_emit(0);
}

static void directive(char *name, char *text)
{
    char *args[4];
    unsigned n, i, width = 0;
    struct symbol *s;
    if (!strcmp(name, ".ascii") || !strcmp(name, ".asciz") || !strcmp(name, ".string")) {
        string_data(text, strcmp(name, ".ascii") != 0);
        return;
    }
    n = split(text, args, 4);
    if (!strcmp(name, ".text") || !strcmp(name, ".data") || !strcmp(name, ".bss")) {
        if (n)
            x64_error("subsections are unsupported");
        x64_current_section = !strcmp(name, ".text") ? TEXT : !strcmp(name, ".data") ? DATA : BSS;
        return;
    }
    if (!strcmp(name, ".section")) {
        if (!n)
            x64_error("missing section name");
        for (i = 0; i < NSECTIONS; i++)
            if (!strcmp(args[0], x64_sections[i].name)) {
                const char *flags = i == TEXT ? "\"ax\"" : i == RODATA ? "\"a\"" : "\"aw\"";
                const char *type = i == BSS ? "@nobits" : "@progbits";
                if (n > 3 || (n > 1 && strcmp(args[1], flags)) || (n > 2 && strcmp(args[2], type)))
                    x64_error("unsupported section flags/type");
                x64_current_section = i;
                return;
            }
        if (!strcmp(args[0], ".note.GNU-stack")) {
            if (n > 3 || (n > 1 && strcmp(args[1], "\"\"")) ||
                (n > 2 && strcmp(args[2], "@progbits")))
                x64_error("unsupported stack-note attributes");
            x64_current_section = NSECTIONS;
            return;
        }
        x64_error("unsupported section '%s'", args[0]);
    }
    if (!strcmp(name, ".globl") || !strcmp(name, ".global") || !strcmp(name, ".local")) {
        if (!n)
            x64_error("missing symbol");
        for (i = 0; i < n; i++) {
            s = lookup(args[i]);
            s->global = strcmp(name, ".local") != 0;
            s->local = !s->global;
        }
        return;
    }
    if (!strcmp(name, ".type")) {
        if (n != 2)
            x64_error(".type needs symbol and type");
        s = lookup(args[0]);
        if (!strcmp(args[1], "@function"))
            s->type = 2;
        else if (!strcmp(args[1], "@object"))
            s->type = 1;
        else
            x64_error("unsupported symbol type");
        return;
    }
    if (!strcmp(name, ".size")) {
        if (n != 2)
            x64_error(".size needs symbol and size");
        s = lookup(args[0]);
        if (!strncmp(args[1], ".-", 2)) {
            if (!s->defined || s->section != x64_current_section ||
                strcmp(x64_trim(args[1] + 2), args[0]))
                x64_error("unsupported size expression");
            s->size = x64_sections[x64_current_section].size - s->value;
        } else {
            U64 size = constant(args[1]);
            if (size > 0xffffffffUL)
                x64_error("symbol size overflow");
            s->size = (U32)size;
        }
        return;
    }
    if (!strcmp(name, ".p2align") || !strcmp(name, ".balign")) {
        U64 alignment, fill = x64_current_section == TEXT ? 0x90 : 0, maximum = 0;
        if (!n)
            x64_error("missing alignment");
        alignment = constant(args[0]);
        if (!strcmp(name, ".p2align")) {
            if (alignment > 12)
                x64_error("alignment exceeds 4096");
            alignment = (U64)1 << alignment;
        }
        if (n > 1 && *args[1])
            fill = constant(args[1]);
        if (n > 2)
            maximum = constant(args[2]);
        if (n > 3 || fill > 255)
            x64_error("invalid alignment operands");
        align_to(alignment, (unsigned)fill, maximum);
        return;
    }
    if (!strcmp(name, ".comm")) {
        unsigned previous = x64_current_section;
        long id;
        U64 size;
        if (n < 2 || n > 3)
            x64_error(".comm needs name, size and optional alignment");
        size = constant(args[1]);
        if (size > 16UL * 1024 * 1024)
            x64_error("common symbol exceeds section limit");
        id = x64_symbol_id(args[0]);
        s = &((struct symbol *)x64_symbols.data)[id];
        if (s->defined)
            x64_error("duplicate common symbol");
        x64_current_section = BSS;
        align_to(n == 3 ? constant(args[2]) : 8, 0, 0);
        s->defined = 1;
        s->global = !s->local;
        s->type = 1;
        s->section = BSS;
        s->value = x64_sections[BSS].size;
        s->size = (U32)size;
        while (size--)
            x64_emit(0);
        x64_current_section = previous;
        return;
    }
    if (!strcmp(name, ".byte"))
        width = 1;
    else if (!strcmp(name, ".short") || !strcmp(name, ".word"))
        width = 2;
    else if (!strcmp(name, ".long") || !strcmp(name, ".int"))
        width = 4;
    else if (!strcmp(name, ".quad"))
        width = 8;
    if (width) {
        if (!n)
            x64_error("missing data value");
        for (i = 0; i < n; i++)
            x64_emit_value(x64_expression(args[i]), width,
                           width == 8   ? 1
                           : width == 4 ? 10
                           : width == 2 ? 12
                                        : 14);
        return;
    }
    if (!strcmp(name, ".zero") || !strcmp(name, ".space") || !strcmp(name, ".fill")) {
        U64 count, size = 1, value = 0, j;
        if (!n)
            x64_error("missing fill count");
        count = constant(args[0]);
        if (!strcmp(name, ".fill")) {
            if (n > 1)
                size = constant(args[1]);
            if (n > 2)
                value = constant(args[2]);
        } else if (n > 1)
            value = constant(args[1]);
        if (strcmp(name, ".fill") && (n > 2 || value > 255))
            x64_error("invalid space/fill octet");
        if (!size || size > 8 || count > 16UL * 1024 * 1024 / size || n > 3)
            x64_error("invalid fill span");
        for (j = 0; j < count; j++)
            x64_emit_number(value, (unsigned)size);
        return;
    }
    /* Debug locations do not affect code or the object symbol/relocation ABI. */
    if (!strcmp(name, ".file") || !strcmp(name, ".loc"))
        return;
    x64_error("unsupported directive '%s'", name);
}

static void line(char *text)
{
    char *s = x64_trim(text), *end, *args[3];
    struct x64_operand operands[3];
    unsigned n, i;
    if (!*s)
        return;
    end = s;
    while (isalnum((unsigned char)*end) || (*end && strchr("_.$", *end)))
        end++;
    if (*end == ':') {
        long id;
        struct symbol *sym;
        if (isdigit((unsigned char)*s) || x64_current_section >= NSECTIONS)
            x64_error("invalid label");
        *end = 0;
        id = x64_symbol_id(s);
        sym = &((struct symbol *)x64_symbols.data)[id];
        if (sym->defined)
            x64_error("duplicate label '%s'", s);
        sym->defined = 1;
        sym->section = x64_current_section;
        sym->value = x64_sections[x64_current_section].size;
        s = x64_trim(end + 1);
        if (!*s)
            return;
    }
    end = s;
    while (*end && !isspace((unsigned char)*end))
        end++;
    if (*end)
        *end++ = 0;
    end = x64_trim(end);
    if (*s == '.')
        directive(s, end);
    else {
        n = split(end, args, 3);
        for (i = 0; i < n; i++)
            operands[i] = x64_operand(args[i]);
        x64_instruction(s, operands, n);
    }
}

void x64_assemble(FILE *input)
{
    struct buffer text = {0};
    int c;
    unsigned quote = 0, escape = 0, slash = 0;
    x64_source_line = 1;
    while ((c = getc(input)) != EOF) {
        if (c > 255)
            x64_error("assembly input contains a non-octet cell");
        if (c == '\n') {
            if (quote)
                x64_error("unterminated string");
            x64_byte(&text, 0);
            line((char *)text.data);
            text.size = 0;
            x64_source_line++;
            slash = escape = 0;
            continue;
        }
        if (block_comment) {
            if (slash && c == '/')
                block_comment = 0;
            slash = c == '*';
            continue;
        }
        if (!quote && c == '#') {
            while ((c = getc(input)) != EOF && c != '\n') {
            }
            if (c == '\n')
                ungetc(c, input);
            continue;
        }
        if (!quote && slash && c == '*') {
            text.size--;
            block_comment = 1;
            slash = 0;
            continue;
        }
        slash = !quote && c == '/';
        x64_byte(&text, (unsigned)c);
        if (c == '"' && !escape)
            quote = !quote;
        escape = quote && c == '\\' && !escape;
        if (text.size > 65535UL)
            x64_error("assembly line exceeds 65535 octets");
    }
    if (ferror(input))
        x64_error("cannot read assembly");
    if (block_comment || quote)
        x64_error("unterminated comment or string");
    if (text.size) {
        x64_byte(&text, 0);
        line((char *)text.data);
    }
    free(text.data);
}
