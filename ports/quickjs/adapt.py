#!/usr/bin/env python3
"""Generate the C16 support-library and embedding/platform experiments.

The engine itself is NOT ported by these transformations. Pristine sources
are verified before use.
"""
import argparse
from pathlib import Path
import re
from vendor import PORT, verify


def replace(text, before, after, count=1):
    if text.count(before) != count:
        raise ValueError('Upstream adaptation anchor changed: ' + before[:80])
    return text.replace(before, after)


def function_body(text, name, body):
    match = re.search(r'\b' + re.escape(name) + r'\([^;{}]*\)\s*\{', text)
    if not match:
        raise ValueError('Missing function definition: ' + name)
    start, end, depth = match.end(), match.end(), 1
    while depth:
        if text[end] == '{': depth += 1
        if text[end] == '}': depth -= 1
        end += 1
    return text[:start] + '\n' + body + '\n' + text[end - 1:]


_C_TOKENS = re.compile(r'''/\*.*?\*/|//[^\n]*|\\\r?\n|"(?:\\.|[^"\\])*"|'(?:\\.|[^'\\])*'|[A-Za-z_]\w*|[^\s]''', re.S)


def source_types(text, widen):
    # Transform identifiers, never JavaScript data embedded in C literals,
    # include paths, character constants or comments. Comments and escaped
    # newlines do not split a compound C type.
    tokens = [token for token in _C_TOKENS.finditer(text)
              if not token[0].startswith(('/*', '//', '\\\n', '\\\r\n'))]
    changes = []
    qualifiers = ('const', 'volatile', 'restrict')
    for i, token in enumerate(tokens):
        word = token[0]
        replacement = {'uint8_t': 'qjs_octet_t',
                       'int8_t': 'qjs_signed_octet_carrier_t'}.get(word, word)
        if widen and word == 'int':
            previous = i - 1
            while previous >= 0 and tokens[previous][0] in qualifiers: previous -= 1
            if previous >= 0 and tokens[previous][0] in ('long', 'short'):
                raise ValueError('Unexpected compound integer spelling')
            replacement = 'long'
        elif widen and word == 'unsigned':
            following = i + 1
            while following < len(tokens) and tokens[following][0] in qualifiers: following += 1
            if following == len(tokens) or tokens[following][0] not in ('char', 'short', 'long', 'int'):
                replacement = 'unsigned long'
        if replacement != word:
            changes.append((token.start(), token.end(), replacement))
    result, start = [], 0
    for begin, end, replacement in changes:
        result.extend((text[start:begin], replacement))
        start = end
    result.append(text[start:])
    return ''.join(result)


def octet_names(text):
    return source_types(text, False)


def private_ints(text):
    # These pinned translation units use int for 32-bit arithmetic. Never
    # apply this to SDK headers; libc output-pointer ABIs must stay intact.
    return source_types(text, True)


def prepare(destination):
    verify()
    destination = Path(destination)
    destination.mkdir(parents=True, exist_ok=True)
    h = (PORT / 'upstream/cutils.h').read_text()
    c = (PORT / 'upstream/cutils.c').read_text()
    # This utility translation unit uses C int as a 32-bit arithmetic/index
    # type. Widen its own declarations, never libc or SDK declarations.
    # cutils' external libc calls take no int* output parameters.
    h, c = [private_ints(t) for t in (h, c)]
    h = replace(h, '#include <inttypes.h>', '#include <inttypes.h>\n#include "c16.h"')
    start = h.index('struct __attribute__((packed)) packed_u64')
    end = h.index('#ifndef bswap16', start)
    h = h[:start] + '#include "octets.inc"\n\n' + h[end:]
    for name, width, direction in [('clz32', 32, 'left'), ('clz64', 64, 'left'),
                                    ('ctz32', 32, 'right'), ('ctz64', 64, 'right')]:
        if direction == 'left':
            body = f'    long n = 0;\n    while (!(a & ((uint{width}_t)1 << {width - 1}))) {{ ++n; a <<= 1; }}\n    return n;'
        else:
            body = '    long n = 0;\n    while (!(a & 1)) { ++n; a >>= 1; }\n    return n;'
        h = function_body(h, name, body)
    for width in (16, 32, 64):
        c = function_body(c, f'__dbuf_put_u{width}',
                          f'    qjs_octet_t octets[{width // 8}];\n'
                          f'    put_u{width}(octets, val);\n'
                          f'    return dbuf_put(s, octets, {width // 8});')
    # A cell-copy swap is valid for all C16 object sizes and alignments.
    c = function_body(c, 'exchange_func', '    (void)base; (void)size;\n    return exchange_bytes;')
    h = replace(h, 's->buf[s->size++] = val;', 's->buf[s->size++] = val & 255U;')
    c = function_body(c, '__dbuf_putc', '    c &= 255U;\n    return dbuf_put(s, &c, 1);')
    # Disable GNU-only attributes/builtins in the private utility header.
    for before, after in [
        ('#define likely(x)       __builtin_expect(!!(x), 1)', '#define likely(x) (!!(x))'),
        ('#define unlikely(x)     __builtin_expect(!!(x), 0)', '#define unlikely(x) (!!(x))'),
        ('#define force_inline inline __attribute__((always_inline))', '#define force_inline inline'),
        ('#define no_inline __attribute__((noinline))', '#define no_inline'),
        ('#define __maybe_unused __attribute__((unused))', '#define __maybe_unused'),
    ]:
        h = replace(h, before, after)
    for text_name in ('h', 'c'):
        text = h if text_name == 'h' else c
        text = text.replace('__attribute__((format(printf, 2, 3))) ', '')
        if text_name == 'h': h = text
        else: c = text
    q = octet_names((PORT / 'upstream/quickjs.h').read_text())
    q = replace(q, '#include <stdint.h>', '#include <stdint.h>\n#include "c16.h"')
    q = replace(q, '    int ref_count;', '    int32_t ref_count;')
    q = replace(q, '#define JS_VALUE_GET_TAG(v) (int)((v) >> 32)',
                '#define JS_VALUE_GET_TAG(v) (int32_t)((v) >> 32)')
    q = replace(q, '    int tag = JS_VALUE_GET_TAG(v);',
                '    int32_t tag = JS_VALUE_GET_TAG(v);', count=2)
    q = replace(q, '#define JS_VALUE_GET_INT(v) (int)(v)', '#define JS_VALUE_GET_INT(v) (int32_t)(v)')
    q = replace(q, '#define JS_VALUE_GET_SHORT_BIG_INT(v) (int)(v)',
                '#define JS_VALUE_GET_SHORT_BIG_INT(v) (int32_t)(v)')
    q = replace(q, '#define JS_TAG_IS_FLOAT64(tag) ((unsigned)((tag) - JS_TAG_FIRST)',
                '#define JS_TAG_IS_FLOAT64(tag) ((uint32_t)((tag) - JS_TAG_FIRST)')
    q = replace(q, '#define JS_VALUE_HAS_REF_COUNT(v) ((unsigned)JS_VALUE_GET_TAG(v) >= (unsigned)JS_TAG_FIRST)',
                '#define JS_VALUE_HAS_REF_COUNT(v) ((uint32_t)JS_VALUE_GET_TAG(v) >= (uint32_t)JS_TAG_FIRST)')
    # The embedding ABI needs the same 32-bit counts, flags and callback
    # output parameters as the engine. SDK declarations remain untouched.
    q = private_ints(q)
    q = re.sub(r'\b([0-3]) <<', r'\1L <<', q)
    q = replace(q, '#define JS_DEFAULT_STACK_SIZE (1024 * 1024)',
                '#define JS_DEFAULT_STACK_SIZE 16384UL /* C16 cells */')
    dtoa = private_ints((PORT / 'upstream/dtoa.c').read_text())
    dtoa_header = private_ints((PORT / 'upstream/dtoa.h').read_text())
    # size_t measures cells; a uint64_t occupies four, not eight, cells.
    dtoa = replace(dtoa, '*pptr += (size + 7) / 8;',
                   '*pptr += (size + sizeof(**pptr) - 1) / sizeof(**pptr);')
    # Keep explicit quiet-NaN bits on invalid input, independent of the SDK
    # macro's spelling and type; the shared SDK constant is now fixed too.
    dtoa = replace(dtoa, 'dval = NAN;', 'dval = uint64_as_float64(0x7ff8000000000000ULL);')
    dtoa = replace(dtoa, 'printf("%08x", a->tab[i]);', 'printf("%08" PRIx32, a->tab[i]);')
    for name, text in [('cutils.h', h), ('cutils.c', c), ('quickjs.h', q),
                       ('dtoa.c', dtoa), ('dtoa.h', dtoa_header)]:
        (destination / name).write_text(text)
    for name in ('c16.h', 'octets.inc', 'inttypes.h', 'qjs-platform.h', 'qjs-platform.c'):
        (destination / name).write_bytes((PORT / 'config' / name).read_bytes())
    from support_adapt import adapt_support
    adapt_support(destination)
    # A code-point workspace stores native uint32_t objects, whereas bytecode
    # operands deliberately retain four logical octet cells per value.
    h = replace(h, '#endif  /* CUTILS_H */', '''\nstatic inline long qjs_dbuf_point(DynBuf *buffer, uint32_t value)
{
    return dbuf_put(buffer, (const qjs_octet_t *)&value, sizeof(value));
}
#endif  /* CUTILS_H */
''')
    (destination / 'cutils.h').write_text(h)
    return destination


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('destination', type=Path)
    prepare(parser.parse_args().destination)
