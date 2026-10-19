"""Checked C16 adaptations for the pinned Unicode and regexp libraries."""
import re
from adapt import PORT, private_ints, octet_names, replace


def adapt_support(destination):
    unicode = private_ints((PORT / 'upstream/libunicode.c').read_text())
    unicode_h = private_ints((PORT / 'upstream/libunicode.h').read_text())
    regexp = private_ints((PORT / 'upstream/libregexp.c').read_text())
    regexp_h = private_ints((PORT / 'upstream/libregexp.h').read_text())
    # Widening declarations does not widen integer promotions of octet cells.
    unicode = replace(unicode, 'ptr[0] | (ptr[1] << 8) | (ptr[2] << 16)',
                      '(uint32_t)ptr[0] | ((uint32_t)ptr[1] << 8) | ((uint32_t)ptr[2] << 16)')
    unicode = replace(unicode, 'p[0] | (p[1] << 8)', 'get_u16(p)')
    unicode = replace(unicode, '(p[0] << 8)', '((uint32_t)p[0] << 8)', 2)
    unicode = replace(unicode, '*p++ << 8', '(uint32_t)*p++ << 8', 4)
    unicode = replace(unicode, 'tab[i++] << 8', '(uint32_t)tab[i++] << 8')
    unicode = replace(unicode, '(((d[p + (k / 4)] >> ((k % 4) * 2)) & 3) << 16)',
                      '(((uint32_t)(d[p + (k / 4)] >> ((k % 4) * 2)) & 3) << 16)')
    unicode = re.sub(r'\b1U?\s*<<', '1UL <<', unicode)
    regexp = re.sub(r'\b1U?\s*<<', '1UL <<', regexp)
    # Native uint32_t workspaces and serialized four-octet operands are distinct.
    unicode = replace(unicode, 'dbuf_put_u32(dbuf,', 'qjs_dbuf_point(dbuf,', 4)
    regexp = replace(regexp, 'dbuf_put_u32(&str, c)', 'qjs_dbuf_point(&str, c)')
    regexp = replace(regexp, 'str.size / 4, (uint32_t *)str.buf',
                     'str.size / sizeof(uint32_t), (uint32_t *)str.buf')
    # Enum arguments are still C16 int at a variadic call site. Make their
    # promoted types match the private library's now-32-bit va_arg operations.
    unicode, programs = re.subn(r'unicode_prop_ops\(cr,([^;]+?)\);',
                               lambda m: 'unicode_prop_ops(cr, ' + ', '.join(
                                   '(long)(' + a.strip() + ')' for a in m[1].split(',')) + ');', unicode)
    if programs != 17:
        raise ValueError('Upstream Unicode property programs changed')
    # Upstream's script OOM label loops back onto itself and can free twice.
    unicode = replace(unicode, '    goto fail;\n}\n\n#define M(id)',
                      '    return -1;\n}\n\n#define M(id)')
    unicode = replace(unicode, '    fail:\n        *pdst = NULL;',
                      '    fail:\n        dbuf_free(dbuf);\n        *pdst = NULL;')
    # No pointer bits are stolen. Cell size and fat-pointer representation must
    # not decide how many backtracking frames can be represented.
    start = regexp.index('typedef union {\n    qjs_octet_t *ptr;')
    end = regexp.index('} StackElem;', start) + len('} StackElem;')
    regexp = regexp[:start] + '''typedef struct {
    qjs_octet_t *ptr;
    intptr_t val;
    struct { uintptr_t val; unsigned type; } bp;
} StackElem;''' + regexp[end:]
    regexp = replace(regexp, '(clen << cbuf_type)',
                     '(clen * (cbuf_type ? sizeof(uint16_t) : 1))')
    regexp = replace(regexp, '(cindex << cbuf_type)',
                     '(cindex * (cbuf_type ? sizeof(uint16_t) : 1))')
    regexp = replace(regexp, 's->group_name_scope++;',
                     's->group_name_scope = (s->group_name_scope + 1) & 255;')
    regexp = replace(regexp, '__attribute__((format(printf, 2, 3))) ', '')
    regexp = replace(regexp, '"expecting \'%c\'", c)', '"expecting \'%c\'", (qjs_c_int)c)')
    for name, text in (('libunicode.h', unicode_h), ('libregexp.h', regexp_h)):
        text = replace(text, '#include <stdint.h>', '#include <stdint.h>\n#include "c16.h"')
        (destination / name).write_text(text)
    for name, text in (('libunicode.c', unicode), ('libregexp.c', regexp)):
        (destination / name).write_text(text)
    (destination / 'libunicode-table.h').write_text(
        octet_names((PORT / 'upstream/libunicode-table.h').read_text()))
    (destination / 'libregexp-opcode.h').write_bytes((PORT / 'upstream/libregexp-opcode.h').read_bytes())
