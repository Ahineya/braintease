#include "syma.h"
#include "cutils.h"
#include "dtoa.h"
#include "primitive-ids.h"
#include "libunicode.h"
#include <ctype.h>
#include <time.h>
static Id boolean(int b) { return syma_sym(b ? "True" : "False"); }
static int eq(Id a, Id b) {
    Count i;
    Node *x = syma_node(a), *y = syma_node(b);
    if (!x || !y)
        return a == b;
    if (x->kind != y->kind)
        return 0;
    if (x->kind != CALL)
        return a == b;
    if (!x->h || !y->h)
        return a == b;
    if (syma_node(x->h)->kind == CALL || syma_node(y->h)->kind == CALL)
        return 0;
    if (x->h != y->h || syma_argcnt(a) != syma_argcnt(b))
        return 0;
    for (i = 0; i < syma_argcnt(a); i++)
        if (!eq(syma_arg(a, i), syma_arg(b, i)))
            return 0;
    return 1;
}
static Id stringof(Id x, unsigned compounds) {
    Node *p = syma_node(x);
    char *s;
    Id r;
    if (p->kind == STR)
        return x;
    if (p->kind == SYM)
        return syma_atom(STR, p->s, p->len);
    if (p->kind == NUM || (p->kind == CALL && compounds)) {
        s = syma_show(x, 0);
        r = syma_str(s);
        free(s);
        return r;
    }
    return 0;
}
static Id joinunits(Id a, Id b) {
    Node *x = syma_node(a), *y = syma_node(b);
    Unit *s = syma_mem((x->len + y->len + 1) * sizeof(Unit));
    Id r;
    memcpy(s, x->s, x->len * sizeof(Unit));
    memcpy(s + x->len, y->s, y->len * sizeof(Unit));
    r = syma_atom(STR, s, x->len + y->len);
    free(s);
    return r;
}
static long findstr(Node *s, Node *needle, Count start) {
    Count i;
    for (i = start; i <= s->len; i++) {
        if (needle->len > s->len - i)
            return -1;
        if (!memcmp(s->s + i, needle->s, needle->len * sizeof(Unit)))
            return (long)i;
    }
    return -1;
}
static uint32_t touint(double d) {
    double v;
    if (!isfinite(d) || !d)
        return 0;
    v = fmod(d < 0 ? ceil(d) : floor(d), 4294967296.0);
    if (v < 0)
        v += 4294967296.0;
    return (uint32_t)v;
}

static const char *primitive_names[] = {NULL,
                                        "Add",
                                        "+",
                                        "Sub",
                                        "-",
                                        "Mul",
                                        "*",
                                        "Div",
                                        "/",
                                        "Mod",
                                        "%",
                                        "Pow",
                                        "^",
                                        "Lt",
                                        "<",
                                        "Gt",
                                        ">",
                                        "Lte",
                                        "<=",
                                        "Gte",
                                        ">=",
                                        "BitAnd",
                                        "&",
                                        "BitOr",
                                        "|",
                                        "BitXor",
                                        "BitShiftLeft",
                                        "<<",
                                        "BitShiftRight",
                                        ">>",
                                        "BitShiftRightUnsigned",
                                        ">>>",
                                        "Sqrt",
                                        "Abs",
                                        "Floor",
                                        "Ceil",
                                        "Round",
                                        "BitNot",
                                        "~",
                                        "CharFromCode",
                                        "Min",
                                        "Max",
                                        "Eq",
                                        "Is",
                                        "==",
                                        "Neq",
                                        "IsNot",
                                        "!=",
                                        "NormalEq",
                                        "Are",
                                        "AreNot",
                                        "AreAny",
                                        "AreAnyNot",
                                        "IsAny",
                                        "AreIn",
                                        "And",
                                        "Or",
                                        "Not",
                                        "If",
                                        "IsNum",
                                        "IsStr",
                                        "IsSym",
                                        "IsComp",
                                        "IsTrue",
                                        "IsFalse",
                                        "Size",
                                        "AreNums",
                                        "AreStrings",
                                        "AreSyms",
                                        "ToString",
                                        "Str",
                                        "ToNormalString",
                                        "Sym",
                                        "Concat",
                                        "Join",
                                        "StrLen",
                                        "Length",
                                        "SplitToChars",
                                        "ToUpper",
                                        "ToLower",
                                        "Trim",
                                        "ParseNum",
                                        "ToNumber",
                                        "Escape",
                                        "Unescape",
                                        "Substring",
                                        "Slice",
                                        "IndexOf",
                                        "Split",
                                        "SplitBy",
                                        "Replace",
                                        "ReplaceAll",
                                        "Serialize",
                                        "Deserialize",
                                        "Splat",
                                        "...!",
                                        "Reverse",
                                        "FreshId",
                                        "Random",
                                        "Debug"};
const unsigned syma_primitive_info[] = {0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 4, 0, 4, 0, 4, 0, 4,
                                        0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 1, 0, 0, 0,
                                        1, 1, 2, 2, 2, 2, 2, 2, 0, 2, 2, 2, 2, 0, 0, 0, 0, 0, 0, 4,
                                        4, 4, 0, 4, 4, 1, 0, 0, 0, 1, 1, 1, 1, 1, 1, 5, 1, 1, 1, 1,
                                        1, 1, 1, 1, 1, 5, 1, 5, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0};
static const char *special_names[] = {NULL,       "Var", "VarRest", "GreedyAnchor", "/^",
                                      "/|",       "/!",  "/+",      "/r/",          "Inert",
                                      "MetaSafe", "/",   "Unbound", "$splice"};
unsigned syma_primitive_id(const Unit *text, Count n) {
    unsigned op;
    Count i;
    for (op = 1; op < sizeof(primitive_names) / sizeof(*primitive_names); op++) {
        const char *name = primitive_names[op];
        for (i = 0; i < n && name[i] && text[i] == (unsigned char)name[i]; i++) {
        }
        if (i == n && !name[i])
            return op;
    }
    for (op = 1; op < sizeof(special_names) / sizeof(*special_names); op++) {
        const char *name = special_names[op];
        for (i = 0; i < n && name[i] && text[i] == (unsigned char)name[i]; i++) {
        }
        if (i == n && !name[i])
            return 256 + op;
    }
    return 0;
}
static unsigned primitive_info(Id h) {
    return h && syma_node(h)->kind == SYM && syma_node(h)->primitive < 256
               ? syma_primitive_info[syma_node(h)->primitive]
               : 0;
}
static int structural(Id h) { return primitive_info(h) & 2; }
Id syma_primitive(Engine *e, Id x, unsigned meta) {
    Id h = syma_node(x)->h, a = syma_arg(x, 0), b = syma_arg(x, 1), c = syma_arg(x, 2), r, v;
    Node *na = syma_node(a), *nb = syma_node(b);
    Count n = syma_argcnt(x), i, j, k;
    Seq *s = NULL;
    double aa = na && na->kind == NUM ? na->num : 0, bb = nb && nb->kind == NUM ? nb->num : 0;
    int both = n == 2 && na && nb && na->kind == NUM && nb->kind == NUM;
    char *text;
    Unit *u;
    uint32_t out[3], cp, ua, ub;
    long pos;
    Engine sub;
    unsigned mode;

    unsigned opcode = h && syma_node(h)->kind == SYM ? syma_node(h)->primitive : 0;
    if (!opcode || opcode >= 256)
        return x;
    if (meta && !(syma_primitive_info[opcode] & 1))
        return x;
    if (both) {
        if ((opcode == SYMA_P_ADD) || (opcode == SYMA_P_PLUS))
            return syma_number(aa + bb);
        if ((opcode == SYMA_P_SUB) || (opcode == SYMA_P_MINUS))
            return syma_number(aa - bb);
        if ((opcode == SYMA_P_MUL) || (opcode == SYMA_P_TIMES))
            return syma_number(aa * bb);
        if ((opcode == SYMA_P_DIV) || (opcode == SYMA_P_SLASH))
            return bb ? syma_number(aa / bb) : x;
        if ((opcode == SYMA_P_MOD) || (opcode == SYMA_P_PERCENT))
            return bb ? syma_number(fmod(aa, bb)) : x;
        if ((opcode == SYMA_P_POW) || (opcode == SYMA_P_CARET))
            return syma_number(pow(aa, bb));
        if ((opcode == SYMA_P_LT) || (opcode == SYMA_P_LT_ALIAS))
            return boolean(aa < bb);
        if ((opcode == SYMA_P_GT) || (opcode == SYMA_P_GT_ALIAS))
            return boolean(aa > bb);
        if ((opcode == SYMA_P_LTE) || (opcode == SYMA_P_LE_ALIAS))
            return boolean(aa <= bb);
        if ((opcode == SYMA_P_GTE) || (opcode == SYMA_P_GE_ALIAS))
            return boolean(aa >= bb);
        ua = touint(aa);
        ub = touint(bb);
        if ((opcode == SYMA_P_BIT_AND) || (opcode == SYMA_P_AMPERSAND))
            return syma_number((int32_t)(ua & ub));
        if ((opcode == SYMA_P_BIT_OR) || (opcode == SYMA_P_PIPE))
            return syma_number((int32_t)(ua | ub));
        if (opcode == SYMA_P_BIT_XOR)
            return syma_number((int32_t)(ua ^ ub));
        if ((opcode == SYMA_P_BIT_SHIFT_LEFT) || (opcode == SYMA_P_LSHIFT))
            return syma_number((int32_t)(ua << (ub & 31)));
        if ((opcode == SYMA_P_BIT_SHIFT_RIGHT) || (opcode == SYMA_P_RSHIFT)) {
            unsigned sh = (unsigned)(ub & 31);
            uint32_t bits = ua >> sh;
            if (sh && (ua & 0x80000000UL))
                bits |= 0xffffffffUL << (32 - sh);
            return syma_number((int32_t)bits);
        }
        if ((opcode == SYMA_P_BIT_SHIFT_RIGHT_UNSIGNED) || (opcode == SYMA_P_URSHIFT))
            return syma_number(ua >> (ub & 31));
    }
    if (n == 1 && na->kind == NUM) {
        if (opcode == SYMA_P_SQRT)
            return aa < 0 ? x : syma_number(sqrt(aa));
        if (opcode == SYMA_P_ABS)
            return syma_number(fabs(aa));
        if (opcode == SYMA_P_FLOOR)
            return syma_number(floor(aa));
        if (opcode == SYMA_P_CEIL)
            return syma_number(ceil(aa));
        if (opcode == SYMA_P_ROUND)
            return syma_number(floor(aa + 0.5));
        if ((opcode == SYMA_P_BIT_NOT) || (opcode == SYMA_P_TILDE))
            return syma_number((int32_t)~touint(aa));
        if ((opcode == SYMA_P_CHAR_FROM_CODE) && aa >= 0 && aa <= 0x10ffff) {
            Unit ch = (Unit)touint(aa);
            return syma_atom(STR, &ch, 1);
        }
    }
    if (((opcode == SYMA_P_MIN) || (opcode == SYMA_P_MAX)) && n) {
        double result = aa;
        for (i = 0; i < n; i++) {
            Node *p = syma_node(syma_arg(x, i));
            if (p->kind != NUM)
                return x;
            if (((opcode == SYMA_P_MIN) && p->num < result) ||
                ((opcode == SYMA_P_MAX) && p->num > result))
                result = p->num;
        }
        return syma_number(result);
    }
    if (n == 2 &&
        ((opcode == SYMA_P_EQ) || (opcode == SYMA_P_IS) || (opcode == SYMA_P_EQ_ALIAS) ||
         (opcode == SYMA_P_NEQ) || (opcode == SYMA_P_IS_NOT) || (opcode == SYMA_P_NE_ALIAS)))
        return boolean(eq(a, b) ^ ((opcode == SYMA_P_NEQ) || (opcode == SYMA_P_IS_NOT) ||
                                   (opcode == SYMA_P_NE_ALIAS)));
    if (n == 2 && (opcode == SYMA_P_NORMAL_EQ)) {
        sub = *e;
        syma_rules_init(&sub.rules);
        if (e->universe)
            syma_rules_extract(&sub.rules, e->universe);
        sub.depth = e->depth + 1;
        sub.excluded = sub.excluded_count = 0;
        sub.trace = 0;
        sub.maxsteps = 10000;
        r = syma_normalize(&sub, a);
        v = syma_normalize(&sub, b);
        syma_rules_free(&sub.rules);
        return boolean(eq(r, v));
    }
    if (n >= 2 &&
        ((opcode == SYMA_P_ARE) || (opcode == SYMA_P_ARE_NOT) || (opcode == SYMA_P_ARE_ANY) ||
         (opcode == SYMA_P_ARE_ANY_NOT) || (opcode == SYMA_P_IS_ANY))) {
        Count same = 0;
        for (i = 1; i < n; i++)
            if (eq(a, syma_arg(x, i)))
                same++;
        return boolean((opcode == SYMA_P_ARE)           ? same == n - 1
                       : (opcode == SYMA_P_ARE_NOT)     ? same == 0
                       : (opcode == SYMA_P_ARE_ANY)     ? same > 0
                       : (opcode == SYMA_P_ARE_ANY_NOT) ? same < n - 1
                                                        : same == 1);
    }
    if (n >= 2 && (opcode == SYMA_P_ARE_IN)) {
        Seq *items = na->kind == CALL ? syma_flat(a) : syma_one(a);
        for (i = 1; i < n; i++) {
            int found = 0;
            for (j = 0; j < syma_length(items); j++)
                if (eq(syma_at(items, j), syma_arg(x, i))) {
                    found = 1;
                    break;
                }
            if (!found)
                return boolean(0);
        }
        return boolean(1);
    }
    if (n && ((opcode == SYMA_P_AND) || (opcode == SYMA_P_OR))) {
        unsigned yes = 0;
        for (i = 0; i < n; i++) {
            v = syma_arg(x, i);
            if (syma_node(v)->kind != SYM)
                return x;
            if (syma_is(v, "True"))
                yes++;
            else if (!syma_is(v, "False"))
                return x;
        }
        return boolean((opcode == SYMA_P_AND) ? yes == n : yes != 0);
    }
    if (n == 1 && (opcode == SYMA_P_NOT) && na->kind == SYM)
        return boolean(!syma_is(a, "True"));
    if (n == 3 && (opcode == SYMA_P_IF) && na->kind == SYM) {
        if (syma_is(a, "True"))
            return b;
        if (syma_is(a, "False"))
            return c;
        return x;
    }
    if (n == 1) {
        if (opcode == SYMA_P_IS_NUM)
            return boolean(na->kind == NUM);
        if (opcode == SYMA_P_IS_STR)
            return boolean(na->kind == STR);
        if (opcode == SYMA_P_IS_SYM)
            return boolean(na->kind == SYM);
        if (opcode == SYMA_P_IS_COMP)
            return boolean(na->kind == CALL);
        if (opcode == SYMA_P_IS_TRUE)
            return boolean(na->kind == SYM && syma_is(a, "True"));
        if (opcode == SYMA_P_IS_FALSE)
            return boolean(na->kind == SYM && syma_is(a, "False"));
        if (opcode == SYMA_P_SIZE)
            return syma_number(na->kind == CALL ? 1 + syma_argcnt(a) : 1);
    }
    if ((opcode == SYMA_P_ARE_NUMS) || (opcode == SYMA_P_ARE_STRINGS) ||
        (opcode == SYMA_P_ARE_SYMS)) {
        unsigned kind = (opcode == SYMA_P_ARE_NUMS)      ? NUM
                        : (opcode == SYMA_P_ARE_STRINGS) ? STR
                                                         : SYM;
        for (i = 0; i < n; i++)
            if (syma_node(syma_arg(x, i))->kind != kind)
                return boolean(0);
        return boolean(1);
    }
    if (n == 1 && ((opcode == SYMA_P_TO_STRING) || (opcode == SYMA_P_STR) ||
                   (opcode == SYMA_P_TO_NORMAL_STRING))) {
        r = stringof(a, (opcode == SYMA_P_TO_STRING));
        return r ? r : x;
    }
    if (n == 1 && (opcode == SYMA_P_SYM) && na->kind == STR && na->len) {
        for (i = 0; i < na->len; i++) {
            unsigned ch = na->s[i];
            if (ch <= 32 || ch == '{' || ch == '}' || ch == '(' || ch == ')' || ch == ',' ||
                ch == ';' || ch == '"' || ch == '\'')
                return x;
        }
        return syma_atom(SYM, na->s, na->len);
    }
    if (((opcode == SYMA_P_CONCAT) && n) || ((opcode == SYMA_P_JOIN) && n && na->kind == STR)) {
        r = syma_str("");
        i = (opcode == SYMA_P_JOIN) ? 1 : 0;
        for (; i < n; i++) {
            v = syma_arg(x, i);
            if (syma_node(v)->kind != STR && syma_node(v)->kind != NUM)
                return x;
            if ((opcode == SYMA_P_JOIN) && i > 1)
                r = joinunits(r, a);
            r = joinunits(r, stringof(v, 0));
        }
        return r;
    }
    if (n == 1 && na->kind == STR) {
        if ((opcode == SYMA_P_STR_LEN) || (opcode == SYMA_P_LENGTH))
            return syma_number(na->len);
        if (opcode == SYMA_P_SPLIT_TO_CHARS) {
            for (i = 0; i < na->len; i++)
                s = syma_cat(s, syma_one(syma_atom(STR, na->s + i, 1)));
            return syma_named("Chars", s);
        }
        if ((opcode == SYMA_P_TO_UPPER) || (opcode == SYMA_P_TO_LOWER)) {
            u = syma_mem((na->len * 3 + 1) * sizeof(Unit));
            k = 0;
            for (i = 0; i < na->len; i++) {
                cp = na->s[i];
                if (cp >= 0xd800 && cp <= 0xdbff && i + 1 < na->len && na->s[i + 1] >= 0xdc00 &&
                    na->s[i + 1] <= 0xdfff)
                    cp = 0x10000 + ((cp - 0xd800) << 10) + (na->s[++i] - 0xdc00);
                mode = (unsigned)lre_case_conv(out, cp, (opcode == SYMA_P_TO_LOWER) ? 1 : 0);
                for (j = 0; j < mode; j++) {
                    cp = out[j];
                    if (cp > 65535) {
                        cp -= 0x10000;
                        u[k++] = (Unit)(0xd800 + (cp >> 10));
                        u[k++] = (Unit)(0xdc00 + (cp & 1023));
                    } else
                        u[k++] = (Unit)cp;
                }
            }
            r = syma_atom(STR, u, k);
            free(u);
            return r;
        }
        if (opcode == SYMA_P_TRIM) {
            i = 0;
            j = na->len;
            while (i < j && (na->s[i] <= 32 || na->s[i] == 160 || na->s[i] == 0xfeff))
                i++;
            while (j > i && (na->s[j - 1] <= 32 || na->s[j - 1] == 160 || na->s[j - 1] == 0xfeff))
                j--;
            return syma_atom(STR, na->s + i, j - i);
        }
        if ((opcode == SYMA_P_PARSE_NUM) || (opcode == SYMA_P_TO_NUMBER)) {
            Count first = 0;
            while (first < na->len && lre_is_space(na->s[first]))
                first++;
            text = syma_utf8(syma_atom(STR, na->s + first, na->len - first));
            const char *next;
            JSATODTempMem temp;
            double d = js_atod(text, &next, 10, 0, &temp);
            r = next != text && !isnan(d) ? syma_number(d) : a;
            free(text);
            return r;
        }
        if (opcode == SYMA_P_ESCAPE) {
            u = syma_mem((na->len * 2 + 1) * sizeof(Unit));
            k = 0;
            for (i = 0; i < na->len; i++) {
                unsigned ch = na->s[i], esc = ch == '\n'   ? 'n'
                                              : ch == '\r' ? 'r'
                                              : ch == '\t' ? 't'
                                              : ch == '\f' ? 'f'
                                              : ch == '"'  ? '"'
                                              : ch == '\\' ? '\\'
                                                           : 0;
                if (esc) {
                    u[k++] = '\\';
                    u[k++] = (Unit)esc;
                } else
                    u[k++] = (Unit)ch;
            }
            r = syma_atom(STR, u, k);
            free(u);
            return r;
        }
        if (opcode == SYMA_P_UNESCAPE) {
            r = a;
            const char *escs = "nrtf\"\\";
            const char *vals = "\n\r\t\f\"\\";
            for (j = 0; j < 6; j++) {
                Node *p = syma_node(r);
                u = syma_mem((p->len + 1) * sizeof(Unit));
                k = 0;
                for (i = 0; i < p->len; i++)
                    if (p->s[i] == '\\' && i + 1 < p->len && p->s[i + 1] == (unsigned)escs[j]) {
                        u[k++] = (Unit)(unsigned char)vals[j];
                        i++;
                    } else
                        u[k++] = p->s[i];
                r = syma_atom(STR, u, k);
                free(u);
            }
            return r;
        }
    }
    if (n >= 2 && na && na->kind == STR &&
        ((opcode == SYMA_P_SUBSTRING) || (opcode == SYMA_P_SLICE)) && nb->kind == NUM) {
        double start = bb, endv = na->len;
        if (start < 0)
            start = fmax(0, (double)na->len + start);
        if (n == 3 && syma_node(c)->kind == NUM) {
            endv = syma_node(c)->num;
            if (endv < 0)
                endv = fmax(0, (double)na->len + endv);
        }
        i = (Count)fmin(na->len, fmax(0, trunc(start)));
        j = (Count)fmin(na->len, fmax(0, trunc(endv)));
        if (i > j) {
            k = i;
            i = j;
            j = k;
        }
        return syma_atom(STR, na->s + i, j - i);
    }
    if (n == 2 && na->kind == STR && nb->kind == STR) {
        if (opcode == SYMA_P_INDEX_OF)
            return syma_number(findstr(na, nb, 0));
        if ((opcode == SYMA_P_SPLIT) || (opcode == SYMA_P_SPLIT_BY)) {
            i = 0;
            if (!na->len) {
                for (j = 0; j < nb->len; j++)
                    s = syma_cat(s, syma_one(syma_atom(STR, nb->s + j, 1)));
            } else {
                while ((pos = findstr(nb, na, i)) >= 0) {
                    s = syma_cat(s, syma_one(syma_atom(STR, nb->s + i, (Count)pos - i)));
                    i = (Count)pos + na->len;
                }
                s = syma_cat(s, syma_one(syma_atom(STR, nb->s + i, nb->len - i)));
            }
            return syma_named("Strings", s);
        }
    }
    if (n == 3 && na->kind == STR && nb->kind == STR && syma_node(c)->kind == STR &&
        ((opcode == SYMA_P_REPLACE) || (opcode == SYMA_P_REPLACE_ALL))) {
        r = syma_str("");
        i = 0;
        while (i <= na->len && (pos = findstr(na, nb, i)) >= 0) {
            r = joinunits(r, syma_atom(STR, na->s + i,
                                       (Count)pos - i)); /* JS replacement string expansions. */
            Node *repl = syma_node(c);
            for (j = 0; j < repl->len; j++) {
                if (repl->s[j] == '$' && j + 1 < repl->len) {
                    unsigned ch = repl->s[j + 1];
                    if (ch == '$') {
                        Unit dollar = '$';
                        r = joinunits(r, syma_atom(STR, &dollar, 1));
                        j++;
                        continue;
                    }
                    if (ch == '&') {
                        r = joinunits(r, b);
                        j++;
                        continue;
                    }
                    if (ch == '`') {
                        r = joinunits(r, syma_atom(STR, na->s, (Count)pos));
                        j++;
                        continue;
                    }
                    if (ch == '\'') {
                        r = joinunits(r, syma_atom(STR, na->s + pos + nb->len,
                                                   na->len - (Count)pos - nb->len));
                        j++;
                        continue;
                    }
                }
                r = joinunits(r, syma_atom(STR, repl->s + j, 1));
            }
            i = (Count)pos + nb->len;
            if (!(opcode == SYMA_P_REPLACE_ALL))
                break;
            if (!nb->len) {
                if (i == na->len) {
                    i++;
                    break;
                }
                r = joinunits(r, syma_atom(STR, na->s + i, 1));
                i++;
            }
        }
        if (i <= na->len)
            r = joinunits(r, syma_atom(STR, na->s + i, na->len - i));
        return r;
    }
    if (n == 1 && (opcode == SYMA_P_SERIALIZE)) {
        text = syma_show(a, 1);
        r = syma_str(text);
        free(text);
        return r;
    }
    if (n == 1 && (opcode == SYMA_P_DESERIALIZE) && na->kind == STR) {
        jmp_buf local;
        jmp_buf *previous = syma_error_target;
        text = syma_utf8(a);
        syma_error_target = &local;
        if (setjmp(local)) {
            syma_error_target = previous;
            free(text);
            return x;
        }
        r = syma_jsonparse(text);
        syma_error_target = previous;
        free(text);
        return r;
    }
    if ((opcode == SYMA_P_SPLAT) || (opcode == SYMA_P_SPREAD))
        return syma_named("$splice", syma_node(x)->a);
    if (opcode == SYMA_P_REVERSE) {
        i = n;
        while (i)
            s = syma_cat(s, syma_one(syma_arg(x, --i)));
        return syma_named("$splice", s);
    }
    if ((opcode == SYMA_P_FRESH_ID) && !n) {
        static Count counter;
        char id[96];
        char *timestamp = syma_show(syma_number(syma_now()), 0);
        sprintf(id, "id_%s_%lu", timestamp, (unsigned long)counter++);
        free(timestamp);
        return syma_str(id);
    }
    if (opcode == SYMA_P_RANDOM) {
        double random = syma_random();
        if (!n)
            return syma_number(random);
        if (n == 2 && both)
            return syma_number(aa + random * (bb - aa));
        return x;
    }
    if ((opcode == SYMA_P_DEBUG) && n) {
        text = syma_show(n > 1 ? b : a, 0);
        fprintf(stderr, "[DEBUG] %s\n", text);
        free(text);
        return n > 1 ? b : a;
    }
    return x;
}
static Id foldrec(Engine *e, Id x, unsigned meta, unsigned depth) {
    Node *p = syma_node(x);
    Id y, h;
    Seq *a = NULL;
    Count i;
    unsigned blocked = 0;
    if (!p || p->kind != CALL || syma_tag(x) == SYMA_T_INERT)
        return x;
    if (depth > 256)
        syma_fail("fold nesting limit exceeded");
    if (p->h && syma_node(p->h)->kind != CALL &&
        (!syma_node(p->h)->primitive || syma_node(p->h)->primitive >= 256) &&
        (!p->a || !(p->a->flags & 2)))
        return x;
    meta |= syma_tag(x) == SYMA_T_META_SAFE;
    if (structural(p->h))
        return syma_primitive(e, x, meta);
    h = foldrec(e, p->h, meta, depth + 1);
    if (!p->a || !(p->a->flags & 2))
        a = p->a;
    else
        for (i = 0; i < syma_argcnt(x); i++) {
            Id v = syma_arg(x, i);
            if ((primitive_info(h) & 4) && syma_tag(v) == SYMA_T_INERT && syma_argcnt(v) == 1)
                v = syma_arg(v, 0);
            y = foldrec(e, v, meta, depth + 1);
            if (syma_tag(y) == SYMA_T_INERT || syma_tag(y) == SYMA_T_META_SAFE)
                blocked = 1;
            a = syma_cat(a, syma_tag(y) == SYMA_T_SPLICE ? syma_node(y)->a : syma_one(y));
        }
    y = syma_call(h, a);
    return blocked ? y : syma_primitive(e, y, meta);
}
Id syma_fold(Engine *e, Id x, unsigned meta) { return foldrec(e, x, meta, 0); }
