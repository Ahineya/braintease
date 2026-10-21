/* x86_64 AT&T encoding for the scalar/SSE2 instructions emitted by QBE.
 * Every unsupported form fails explicitly. Addresses and code bytes are
 * octet quantities, even when this assembler is compiled for 16-bit chars. */
#include "x64.h"
#include <ctype.h>
#include <stdlib.h>
#include <string.h>

static int signed8(U64 n)
{
    return n <= 127 || n >= ~(U64)127;
}

static int signed32(U64 n)
{
    return n <= 2147483647ULL || n >= ~(U64)2147483647;
}

static int reg(char *s, unsigned *width, int *xmm)
{
    static const char *names[4][8] = {{"al", "cl", "dl", "bl", "spl", "bpl", "sil", "dil"},
                                      {"ax", "cx", "dx", "bx", "sp", "bp", "si", "di"},
                                      {"eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi"},
                                      {"rax", "rcx", "rdx", "rbx", "rsp", "rbp", "rsi", "rdi"}};
    unsigned i, j;
    char *end;
    unsigned long n;
    if (*s != '%')
        x64_error("expected register");
    s++;
    *xmm = 0;
    if (!strcmp(s, "rip")) {
        *width = 64;
        return 16;
    }
    if (!strncmp(s, "xmm", 3)) {
        n = strtoul(s + 3, &end, 10);
        if (end == s + 3 || *end || n > 15)
            x64_error("invalid XMM register");
        *width = 128;
        *xmm = 1;
        return (int)n;
    }
    for (i = 0; i < 4; i++)
        for (j = 0; j < 8; j++)
            if (!strcmp(s, names[i][j])) {
                *width = 8U << i;
                return (int)j;
            }
    if (*s == 'r' && isdigit((unsigned char)s[1])) {
        n = strtoul(s + 1, &end, 10);
        if (n < 8 || n > 15)
            x64_error("invalid register");
        if (!*end)
            *width = 64;
        else if (!strcmp(end, "d"))
            *width = 32;
        else if (!strcmp(end, "w"))
            *width = 16;
        else if (!strcmp(end, "b"))
            *width = 8;
        else
            x64_error("invalid register suffix");
        return (int)n;
    }
    x64_error("unsupported register '%s'", s);
    return 0;
}

struct x64_operand x64_operand(char *text)
{
    struct x64_operand o;
    char *s = x64_trim(text), *left, *right, *part[3];
    unsigned n = 0, width;
    int xmm;
    memset(&o, 0, sizeof(o));
    o.reg = o.base = o.index = -1;
    o.scale = 1;
    o.value.symbol = -1;
    if (*s == '*') {
        o.indirect = 1;
        s = x64_trim(s + 1);
    }
    if (*s == '$') {
        o.kind = OIMM;
        o.value = x64_expression(s + 1);
        return o;
    }
    left = strchr(s, '(');
    if (left) {
        o.kind = OMEM;
        *left++ = 0;
        if (*x64_trim(s))
            o.value = x64_expression(s);
        right = strchr(left, ')');
        if (!right || *x64_trim(right + 1))
            x64_error("invalid memory operand");
        *right = 0;
        part[n++] = left;
        for (s = left; *s; s++)
            if (*s == ',') {
                *s = 0;
                if (n == 3)
                    x64_error("too many address components");
                part[n++] = s + 1;
            }
        if (*x64_trim(part[0])) {
            o.base = reg(x64_trim(part[0]), &width, &xmm);
            if (xmm || width != 64)
                x64_error("address base must be a 64-bit register");
        }
        if (n > 1 && *x64_trim(part[1])) {
            o.index = reg(x64_trim(part[1]), &width, &xmm);
            if (xmm || width != 64 || o.index == 16 || o.index == 4)
                x64_error("invalid address index");
        }
        if (n > 2) {
            struct x64_expression scale = x64_expression(part[2]);
            if (scale.symbol >= 0 ||
                !(scale.addend == 1 || scale.addend == 2 || scale.addend == 4 || scale.addend == 8))
                x64_error("invalid address scale");
            o.scale = (int)scale.addend;
        }
        if (o.base == 16 && o.index >= 0)
            x64_error("RIP-relative index is invalid");
        return o;
    }
    if (*s == '%') {
        o.kind = OREG;
        o.reg = reg(s, &o.width, &o.xmm);
        if (o.reg == 16)
            x64_error("RIP is only valid as an address base");
        return o;
    }
    o.kind = OLABEL;
    o.value = x64_expression(s);
    return o;
}

static void prefix(unsigned mandatory, unsigned width, int field, struct x64_operand *rm)
{
    unsigned rex = 64;
    int byte_rex = 0;
    if (mandatory)
        x64_emit(mandatory);
    if (width == 16)
        x64_emit(0x66);
    if (width == 64)
        rex |= 8;
    if (field >= 8)
        rex |= 4;
    if (rm->kind == OREG) {
        if (rm->reg >= 8)
            rex |= 1;
        if (rm->width == 8 && !rm->xmm && rm->reg >= 4)
            byte_rex = 1;
    } else if (rm->kind == OMEM) {
        if (rm->base >= 8 && rm->base != 16)
            rex |= 1;
        if (rm->index >= 8)
            rex |= 2;
    } else
        x64_error("expected register or memory");
    if (width == 8 && field >= 4)
        byte_rex = 1;
    if (rex != 64 || byte_rex)
        x64_emit(rex);
}

static void modrm(int field, struct x64_operand *rm)
{
    int mode, base = rm->base, sib;
    U64 disp = rm->value.addend;
    if (rm->kind == OREG) {
        x64_emit(0xc0 | ((field & 7) << 3) | (rm->reg & 7));
        return;
    }
    if (rm->kind != OMEM)
        x64_error("expected memory operand");
    if (base == 16) {
        x64_emit(((field & 7) << 3) | 5);
        x64_emit_value(rm->value, 4, 2);
        return;
    }
    sib = rm->index >= 0 || base < 0 || (base & 7) == 4;
    if (base < 0)
        mode = 0;
    else if (rm->value.symbol >= 0 || !signed8(disp))
        mode = 2;
    else if (disp || (base & 7) == 5)
        mode = 1;
    else
        mode = 0;
    x64_emit((mode << 6) | ((field & 7) << 3) | (sib ? 4 : base & 7));
    if (sib) {
        unsigned scale = rm->scale == 8 ? 3 : rm->scale == 4 ? 2 : rm->scale == 2 ? 1 : 0;
        x64_emit((scale << 6) | ((rm->index < 0 ? 4 : rm->index & 7) << 3) |
                 (base < 0 ? 5 : base & 7));
    }
    if (mode == 1)
        x64_emit((unsigned)disp & 255);
    if (mode == 2 || base < 0) {
        if (rm->value.symbol < 0 && !signed32(disp))
            x64_error("address displacement exceeds signed 32 bits");
        x64_emit_value(rm->value, 4, 11);
    }
}

static void operation(unsigned mandatory, unsigned width, unsigned opcode, int field,
                      struct x64_operand *rm)
{
    prefix(mandatory, width, field, rm);
    if (opcode > 255)
        x64_emit(opcode >> 8);
    x64_emit(opcode & 255);
    modrm(field, rm);
}

static void immediate(struct x64_expression value, unsigned width)
{
    unsigned bytes = width == 64 ? 4 : width / 8;
    if (value.symbol < 0 && width == 64 && !signed32(value.addend))
        x64_error("immediate exceeds signed 32 bits");
    if (value.symbol < 0 && width < 64 && value.addend > (((U64)1 << width) - 1) &&
        value.addend < ~(((U64)1 << (width - 1)) - 1))
        x64_error("immediate exceeds operand width");
    x64_emit_value(value, bytes, width == 64 ? 11 : width == 32 ? 10 : width == 16 ? 12 : 14);
}

static unsigned integer_width(char *name, struct x64_operand *o, unsigned n)
{
    size_t size = strlen(name);
    char c = size ? name[size - 1] : 0;
    unsigned width = c == 'b' ? 8 : c == 'w' ? 16 : c == 'l' ? 32 : c == 'q' ? 64 : 0, i;
    if (width)
        name[size - 1] = 0;
    else
        for (i = n; i; i--)
            if (o[i - 1].kind == OREG && !o[i - 1].xmm) {
                width = o[i - 1].width;
                break;
            }
    if (!width)
        x64_error("instruction needs an explicit operand width");
    for (i = 0; i < n; i++)
        if (o[i].kind == OREG && (o[i].xmm || o[i].width != width))
            x64_error("inconsistent integer operand widths");
    return width;
}

static int condition(const char *name)
{
    static const char *names[16] = {"o", "no", "b", "ae", "z", "nz", "be", "a",
                                    "s", "ns", "p", "np", "l", "ge", "le", "g"};
    int i;
    if (!strcmp(name, "e"))
        return 4;
    if (!strcmp(name, "ne"))
        return 5;
    if (!strcmp(name, "c") || !strcmp(name, "nae"))
        return 2;
    if (!strcmp(name, "nc") || !strcmp(name, "nb"))
        return 3;
    for (i = 0; i < 16; i++)
        if (!strcmp(name, names[i]))
            return i;
    return -1;
}

static int sse(char *name, struct x64_operand *o, unsigned n)
{
    static const char *math[] = {"add", "mul", "sub", "div", "sqrt", "xor", "and", "or"};
    static const unsigned codes[] = {0x58, 0x59, 0x5c, 0x5e, 0x51, 0x57, 0x54, 0x56};
    unsigned pre = 0, code = 0, width = 0, i;
    size_t len = strlen(name);
    char base[32];
    int store = 0;
    if (n != 2)
        return 0;
    if (!strcmp(name, "movd") || (!strcmp(name, "movq") && ((o[0].kind == OREG && o[0].xmm) ||
                                                            (o[1].kind == OREG && o[1].xmm)))) {
        width = name[3] == 'q' ? 64 : 32;
        for (i = 0; i < 2; i++)
            if (o[i].kind == OREG && !o[i].xmm && o[i].width != width)
                x64_error("SSE move integer width mismatch");
        if (o[1].kind == OREG && o[1].xmm) {
            if (o[0].kind == OREG && o[0].xmm) {
                if (width != 64)
                    x64_error("movd cannot copy XMM to XMM");
                operation(0xf3, 0, 0x0f7e, o[1].reg, &o[0]);
            } else
                operation(0x66, width, 0x0f6e, o[1].reg, &o[0]);
        } else if (o[0].kind == OREG && o[0].xmm)
            operation(0x66, width, 0x0f7e, o[0].reg, &o[1]);
        else
            x64_error("invalid SSE move");
        return 1;
    }
    if (!strncmp(name, "cvttss2si", 9) || !strncmp(name, "cvttsd2si", 9)) {
        if (o[0].kind == OREG && !o[0].xmm)
            x64_error("float conversion requires XMM source");
        if (name[9] && (name[10] || (name[9] != 'q' && name[9] != 'l') ||
                        o[1].width != (name[9] == 'q' ? 64 : 32)))
            x64_error("invalid float conversion suffix");
        if (o[1].kind != OREG || o[1].xmm || (o[1].width != 32 && o[1].width != 64))
            x64_error("invalid float conversion destination");
        operation(name[5] == 's' ? 0xf3 : 0xf2, o[1].width, 0x0f2c, o[1].reg, &o[0]);
        return 1;
    }
    if (!strcmp(name, "cvtsi2ss") || !strcmp(name, "cvtsi2sd")) {
        if (o[0].kind != OREG || o[0].xmm || (o[0].width != 32 && o[0].width != 64) ||
            o[1].kind != OREG || !o[1].xmm)
            x64_error("conversion needs integer and XMM registers");
        operation(name[7] == 's' ? 0xf3 : 0xf2, o[0].width, 0x0f2a, o[1].reg, &o[0]);
        return 1;
    }
    if (!strcmp(name, "cvtss2sd") || !strcmp(name, "cvtsd2ss")) {
        if (o[0].kind == OREG && !o[0].xmm)
            x64_error("conversion needs XMM source");
        if (o[1].kind != OREG || !o[1].xmm)
            x64_error("conversion needs an XMM destination");
        operation(name[4] == 's' ? 0xf3 : 0xf2, 0, 0x0f5a, o[1].reg, &o[0]);
        return 1;
    }
    if (!strcmp(name, "movss") || !strcmp(name, "movsd") || !strcmp(name, "movaps") ||
        !strcmp(name, "movups") || !strcmp(name, "movapd") || !strcmp(name, "movupd")) {
        for (i = 0; i < 2; i++)
            if (o[i].kind == OREG && !o[i].xmm)
                x64_error("SSE move needs XMM registers");
        pre = !strcmp(name, "movss")   ? 0xf3
              : !strcmp(name, "movsd") ? 0xf2
              : strstr(name, "pd")     ? 0x66
                                       : 0;
        code = strstr(name, "movap") ? 0x28 : 0x10;
        store = o[1].kind == OMEM;
        if (store) {
            if (o[0].kind != OREG || !o[0].xmm)
                x64_error("SSE store needs XMM source");
            operation(pre, 0, 0x0f00 | code | 1, o[0].reg, &o[1]);
        } else {
            if (o[1].kind != OREG || !o[1].xmm)
                x64_error("SSE move needs XMM destination");
            operation(pre, 0, 0x0f00 | code, o[1].reg, &o[0]);
        }
        return 1;
    }
    if (!strcmp(name, "ucomiss") || !strcmp(name, "ucomisd")) {
        if (o[0].kind == OREG && !o[0].xmm)
            x64_error("comparison needs XMM source");
        if (o[1].kind != OREG || !o[1].xmm)
            x64_error("comparison needs XMM destination");
        operation(name[6] == 'd' ? 0x66 : 0, 0, 0x0f2e, o[1].reg, &o[0]);
        return 1;
    }
    if (len < 3 || len >= sizeof base)
        return 0;
    strcpy(base, name);
    base[len - 2] = 0;
    if (!strcmp(name + len - 2, "ss"))
        pre = 0xf3;
    else if (!strcmp(name + len - 2, "sd"))
        pre = 0xf2;
    else if (!strcmp(name + len - 2, "pd"))
        pre = 0x66;
    else if (strcmp(name + len - 2, "ps"))
        return 0;
    for (i = 0; i < sizeof codes / sizeof codes[0]; i++)
        if (!strcmp(base, math[i]))
            code = codes[i];
    if (!code)
        return 0;
    if ((code == 0x54 || code == 0x56 || code == 0x57) && (pre == 0xf3 || pre == 0xf2))
        x64_error("SSE bitwise operations require ps/pd suffixes");
    if (o[0].kind == OREG && !o[0].xmm)
        x64_error("SSE operation needs XMM source");
    if (o[1].kind != OREG || !o[1].xmm)
        x64_error("SSE operation needs XMM destination");
    operation(pre, 0, 0x0f00 | code, o[1].reg, &o[0]);
    return 1;
}

static int fixed_instruction(char *name, unsigned n)
{
    if (!strcmp(name, "ret") || !strcmp(name, "retq")) {
        if (n)
            x64_error("ret takes no operands");
        x64_emit(0xc3);
        return 1;
    }
    if (!strcmp(name, "leave")) {
        if (n)
            x64_error("leave takes no operands");
        x64_emit(0xc9);
        return 1;
    }
    if (!strcmp(name, "nop")) {
        if (n)
            x64_error("nop takes no operands");
        x64_emit(0x90);
        return 1;
    }
    if (!strcmp(name, "ud2")) {
        if (n)
            x64_error("ud2 takes no operands");
        x64_emit(0x0f);
        x64_emit(0x0b);
        return 1;
    }
    if (!strcmp(name, "endbr64")) {
        if (n)
            x64_error("endbr64 takes no operands");
        x64_emit(0xf3);
        x64_emit(0x0f);
        x64_emit(0x1e);
        x64_emit(0xfa);
        return 1;
    }
    if (!strcmp(name, "cltd") || !strcmp(name, "cqto") || !strcmp(name, "cltq")) {
        if (n)
            x64_error("sign extension takes no operands");
        if (strcmp(name, "cltd"))
            x64_emit(0x48);
        x64_emit(!strcmp(name, "cltq") ? 0x98 : 0x99);
        return 1;
    }
    if (!strcmp(name, "syscall")) {
        if (n)
            x64_error("syscall takes no operands");
        x64_emit(0x0f);
        x64_emit(0x05);
        return 1;
    }
    if (!strcmp(name, "hlt") || !strcmp(name, "int3") || !strcmp(name, "cld")) {
        if (n)
            x64_error("instruction takes no operands");
        x64_emit(!strcmp(name, "hlt") ? 0xf4 : !strcmp(name, "int3") ? 0xcc : 0xfc);
        return 1;
    }
    return 0;
}

static int control_instruction(char *name, struct x64_operand *o, unsigned n)
{
    int cc;
    if (!strcmp(name, "jmp") || !strcmp(name, "call") || !strcmp(name, "callq") ||
        (name[0] == 'j' && (cc = condition(name + 1)) >= 0)) {
        int call = !strncmp(name, "call", 4);
        if (n != 1)
            x64_error("branch needs one operand");
        if (o[0].indirect) {
            if (o[0].kind == OREG && (o[0].xmm || o[0].width != 64))
                x64_error("indirect branch requires a 64-bit register");
            operation(0, 0, 0xff, call ? 2 : 4, &o[0]);
        } else {
            if (o[0].kind != OLABEL || o[0].value.symbol < 0)
                x64_error("direct branch requires a label");
            if (name[0] == 'j' && strcmp(name, "jmp")) {
                x64_emit(0x0f);
                x64_emit(0x80 + condition(name + 1));
            } else
                x64_emit(call ? 0xe8 : 0xe9);
            x64_emit_value(o[0].value, 4, call ? 4 : 2);
        }
        return 1;
    }
    if (!strncmp(name, "set", 3) && (cc = condition(name + 3)) >= 0) {
        if (n != 1 || (o[0].kind == OREG && (o[0].xmm || o[0].width != 8)))
            x64_error("setcc needs a byte operand");
        operation(0, 8, 0x0f90 + cc, 0, &o[0]);
        return 1;
    }
    if (!strncmp(name, "cmov", 4)) {
        size_t len = strlen(name);
        char saved = name[len - 1];
        unsigned suffix = 0;
        cc = condition(name + 4);
        if (cc < 0 && (saved == 'q' || saved == 'l' || saved == 'w')) {
            name[len - 1] = 0;
            cc = condition(name + 4);
            suffix = saved == 'q' ? 64 : saved == 'l' ? 32 : 16;
        }
        if (cc < 0 || n != 2 || o[1].kind != OREG || o[1].xmm || o[1].width == 8)
            x64_error("invalid conditional move");
        if ((suffix && suffix != o[1].width) ||
            (o[0].kind == OREG && (o[0].xmm || o[0].width != o[1].width)))
            x64_error("conditional move operand width mismatch");
        operation(0, o[1].width, 0x0f40 + cc, o[1].reg, &o[0]);
        return 1;
    }
    return 0;
}

static void integer_instruction(char *name, struct x64_operand *o, unsigned n)
{
    static const char *alu[] = {"add", "or", "adc", "sbb", "and", "sub", "xor", "cmp"};
    unsigned width, i, code = 0;
    width = integer_width(name, o, n);
    if (!strcmp(name, "push") || !strcmp(name, "pop")) {
        if (n != 1 || width != 64)
            x64_error("push/pop require a 64-bit operand");
        if (o[0].kind == OREG) {
            if (o[0].reg >= 8)
                x64_emit(0x41);
            x64_emit((!strcmp(name, "push") ? 0x50 : 0x58) + (o[0].reg & 7));
        } else if (o[0].kind == OIMM && !strcmp(name, "push")) {
            if (signed8(o[0].value.addend) && o[0].value.symbol < 0) {
                x64_emit(0x6a);
                x64_emit((unsigned)o[0].value.addend & 255);
            } else {
                x64_emit(0x68);
                immediate(o[0].value, 64);
            }
        } else
            operation(0, 0, !strcmp(name, "push") ? 0xff : 0x8f, !strcmp(name, "push") ? 6 : 0,
                      &o[0]);
        return;
    }
    if (!strcmp(name, "mov") || !strcmp(name, "movabs")) {
        if (n != 2)
            x64_error("mov needs two operands");
        if (o[0].kind == OIMM) {
            if (o[1].kind == OREG) {
                if (width == 64 && o[0].value.symbol < 0 && signed32(o[0].value.addend)) {
                    operation(0, width, 0xc7, 0, &o[1]);
                    immediate(o[0].value, width);
                } else {
                    prefix(0, width, 0, &o[1]);
                    x64_emit((width == 8 ? 0xb0 : 0xb8) + (o[1].reg & 7));
                    x64_emit_value(o[0].value, width / 8,
                                   width == 64   ? 1
                                   : width == 32 ? 10
                                   : width == 16 ? 12
                                                 : 14);
                }
            } else {
                operation(0, width, width == 8 ? 0xc6 : 0xc7, 0, &o[1]);
                immediate(o[0].value, width);
            }
        } else if (o[1].kind == OREG)
            operation(0, width, width == 8 ? 0x8a : 0x8b, o[1].reg, &o[0]);
        else if (o[0].kind == OREG)
            operation(0, width, width == 8 ? 0x88 : 0x89, o[0].reg, &o[1]);
        else
            x64_error("mov cannot have two memory operands");
        return;
    }
    if (!strcmp(name, "lea")) {
        if (n != 2 || o[0].kind != OMEM || o[1].kind != OREG || width == 8)
            x64_error("invalid lea");
        operation(0, width, 0x8d, o[1].reg, &o[0]);
        return;
    }
    if (!strcmp(name, "imul") && n >= 2) {
        if (width == 8 || n > 3 || o[n - 1].kind != OREG)
            x64_error("invalid multiply");
        if (n == 3) {
            if (o[0].kind != OIMM)
                x64_error("three-operand multiply needs an immediate");
            i = o[0].value.symbol < 0 && signed8(o[0].value.addend);
            operation(0, width, i ? 0x6b : 0x69, o[2].reg, &o[1]);
            if (i)
                x64_emit((unsigned)o[0].value.addend & 255);
            else
                immediate(o[0].value, width);
        } else
            operation(0, width, 0x0faf, o[1].reg, &o[0]);
        return;
    }
    if (!strcmp(name, "not") || !strcmp(name, "neg") || !strcmp(name, "mul") ||
        !strcmp(name, "imul") || !strcmp(name, "div") || !strcmp(name, "idiv") ||
        !strcmp(name, "inc") || !strcmp(name, "dec")) {
        if (n != 1)
            x64_error("unary instruction needs one operand");
        i = !strcmp(name, "not")    ? 2
            : !strcmp(name, "neg")  ? 3
            : !strcmp(name, "mul")  ? 4
            : !strcmp(name, "imul") ? 5
            : !strcmp(name, "div")  ? 6
            : !strcmp(name, "idiv") ? 7
            : !strcmp(name, "inc")  ? 0
                                    : 1;
        operation(0, width, i < 2 ? (width == 8 ? 0xfe : 0xff) : (width == 8 ? 0xf6 : 0xf7), (int)i,
                  &o[0]);
        return;
    }
    if (!strcmp(name, "xchg")) {
        if (n != 2 || o[0].kind != OREG)
            x64_error("invalid xchg");
        operation(0, width, width == 8 ? 0x86 : 0x87, o[0].reg, &o[1]);
        return;
    }
    for (i = 0; i < 8; i++)
        if (!strcmp(name, alu[i]))
            break;
    if (i < 8 || !strcmp(name, "test")) {
        unsigned test = i == 8;
        if (n != 2)
            x64_error("arithmetic needs two operands");
        if (o[0].kind == OIMM) {
            unsigned small =
                !test && width != 8 && o[0].value.symbol < 0 && signed8(o[0].value.addend);
            operation(0, width,
                      test         ? (width == 8 ? 0xf6 : 0xf7)
                      : small      ? 0x83
                      : width == 8 ? 0x80
                                   : 0x81,
                      test ? 0 : (int)i, &o[1]);
            if (small)
                x64_emit((unsigned)o[0].value.addend & 255);
            else
                immediate(o[0].value, width);
        } else if (o[0].kind == OREG)
            operation(0, width, test ? (width == 8 ? 0x84 : 0x85) : i * 8 + (width == 8 ? 0 : 1),
                      o[0].reg, &o[1]);
        else if (o[1].kind == OREG && !test)
            operation(0, width, i * 8 + (width == 8 ? 2 : 3), o[1].reg, &o[0]);
        else
            x64_error("invalid arithmetic operands");
        return;
    }
    x64_error("unsupported instruction '%s'", name);
}

void x64_instruction(char *name, struct x64_operand *o, unsigned n)
{
    unsigned width, i, code = 0;
    size_t first_reloc = x64_relocations.count, k;
    if (x64_current_section != TEXT)
        x64_error("instructions require .text");
    if (fixed_instruction(name, n))
        return;
    if (control_instruction(name, o, n))
        goto finish;
    for (i = 0; i < n; i++)
        if (o[i].kind == OLABEL)
            o[i].kind = OMEM;
    if (sse(name, o, n))
        goto finish;
    if (!strncmp(name, "movs", 4) || !strncmp(name, "movz", 4)) {
        unsigned from = name[4] == 'b' ? 8 : name[4] == 'w' ? 16 : name[4] == 'l' ? 32 : 0;
        size_t len = strlen(name);
        width = name[len - 1] == 'q' ? 64 : name[len - 1] == 'l' ? 32 : 0;
        if (len != 6 || n != 2 || !from || !width || o[1].kind != OREG || o[1].xmm ||
            o[1].width != width || (o[0].kind == OREG && o[0].width != from))
            x64_error("invalid extending move");
        code = from == 32 ? 0x63 : 0x0f00 | (name[3] == 'z' ? 0xb6 : 0xbe) | (from == 16 ? 1 : 0);
        if (from == 32 && name[3] != 's')
            x64_error("invalid extension");
        operation(0, width, code, o[1].reg, &o[0]);
        goto finish;
    }
    /* Shift counts are always byte-sized, independent of destination width. */
    if (!strncmp(name, "shl", 3) || !strncmp(name, "sal", 3) || !strncmp(name, "shr", 3) ||
        !strncmp(name, "sar", 3) || !strncmp(name, "rol", 3) || !strncmp(name, "ror", 3)) {
        struct x64_operand dest;
        if (n != 2)
            x64_error("shift needs two operands");
        dest = o[1];
        width = integer_width(name, &dest, 1);
        i = !strcmp(name, "shl") || !strcmp(name, "sal") ? 4
            : !strcmp(name, "shr")                       ? 5
            : !strcmp(name, "sar")                       ? 7
            : !strcmp(name, "rol")                       ? 0
                                                         : 1;
        if (o[0].kind == OIMM && o[0].value.symbol < 0 && o[0].value.addend <= 255) {
            operation(0, width, width == 8 ? 0xc0 : 0xc1, (int)i, &o[1]);
            x64_emit((unsigned)o[0].value.addend);
        } else if (o[0].kind == OREG && o[0].reg == 1 && o[0].width == 8)
            operation(0, width, width == 8 ? 0xd2 : 0xd3, (int)i, &o[1]);
        else
            x64_error("shift count must be an immediate byte or %%cl");
        goto finish;
    }
    integer_instruction(name, o, n);
finish:
    /* RIP is relative to the END of the instruction, including any trailing
     * immediate; ELF PC-relative relocations use the relocation field's P. */
    for (k = first_reloc; k < x64_relocations.count; k++) {
        struct relocation *r = &((struct relocation *)x64_relocations.data)[k];
        if (r->type == 2 || r->type == 4)
            r->value.addend -= x64_sections[TEXT].size - r->offset;
    }
}
