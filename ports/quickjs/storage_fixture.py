"""Extract pinned production routines for isolated representation tests.

The fixture substitutes only allocation/exception/context plumbing. It does
not instantiate a runtime, execute bytecode, or stand in for an engine port.
"""
import re
from adapt import _C_TOKENS

FUNCTIONS = (
    'string_get', 'js_string_char_size', 'js_string_data_size',
    'js_alloc_string_rt', 'js_alloc_string', 'js_free_string',
    'hash_string8', 'hash_string16', 'hash_string', 'count_ascii',
    'js_new_string8_len', 'js_new_string16_len', 'js_new_string_char',
    'js_sub_string', 'string_buffer_init2', 'string_buffer_init',
    'string_buffer_free', 'string_buffer_set_error', 'string_buffer_widen',
    'string_buffer_realloc', 'string_buffer_putc16_slow', 'string_buffer_putc8',
    'string_buffer_putc16', 'string_buffer_putc_slow', 'string_buffer_putc',
    'string_getc', 'string_buffer_write8', 'string_buffer_write16',
    'string_buffer_puts8', 'string_buffer_concat', 'string_buffer_fill',
    'string_buffer_end', 'JS_NewStringLen', 'JS_ToCStringLen2', 'JS_FreeCString',
    'memcmp16_8', 'memcmp16', 'js_string_memcmp', 'js_string_eq',
    'js_string_compare', 'copy_str16', 'JS_ConcatString1', 'JS_ConcatStringInPlace',
    'string_cmp', 'string_indexof_char', 'string_indexof', 'string_advance_index',
    'js_string_find_invalid_codepoint', 'is_be',
    'dbuf_put_leb128', 'dbuf_put_sleb128', 'get_leb128', 'get_sleb128',
    'bc_put_u8', 'bc_put_u16', 'bc_put_u32', 'bc_put_u64',
    'bc_put_leb128', 'bc_put_sleb128', 'JS_WriteString',
    'bc_read_error_end', 'bc_get_u8', 'bc_get_u16', 'bc_get_u32', 'bc_get_u64',
    'bc_get_leb128', 'bc_get_sleb128', 'JS_ReadString',
)


def definition(source, name):
    match = re.search(r'(?m)^[A-Za-z_][\w *]*?\b' + re.escape(name) + r'\([^;{}]*\)\s*\{', source)
    if not match:
        raise ValueError('Missing storage fixture function: ' + name)
    depth = 1
    for token in _C_TOKENS.finditer(source, match.end()):
        if token[0] == '{': depth += 1
        elif token[0] == '}': depth -= 1
        if not depth:
            return match.start(), source[match.start():token.end()]
    raise ValueError('Unclosed storage fixture function: ' + name)


def fixture(source, adapted):
    string = source[source.index('struct JSString {'):source.index('typedef struct JSStringRope {')]
    start = source.index('typedef struct StringBuffer {')
    buffer = source[start:source.index('} StringBuffer;', start) + len('} StringBuffer;')]
    functions = [definition(source, name) for name in FUNCTIONS
                 if adapted or name not in ('js_string_char_size', 'js_string_data_size')]
    # Use the pinned opcode enum/table without inventing fixture opcode values.
    enum_end = source.index('\n};', source.index('enum OPCodeEnum {')) + len('\n};')
    enums = source[source.index('typedef enum OPCodeFormat {'):enum_end]
    start = source.index('typedef struct JSOpCode {')
    short_end = source.index('#endif\n', source.index('#define short_opcode_info(op) opcode_info[op]', start))
    opcodes = source[start:short_end + len('#endif\n')]
    start = source.index('typedef struct CodeContext {')
    context = source[start:source.index('static BOOL ', start)]
    functions += [definition(source, 'push_short_int'), definition(source, 'put_short_code'),
                  definition(source, 'qjs_code_match' if adapted else 'code_match')]
    # Definitions remain in upstream order, with the StringBuffer type hoisted.
    sections = ['/* Generated from verified upstream; not an engine. */',
                '#define SHORT_OPCODES 1', string, buffer, enums, opcodes, context,
                '\n\n'.join(text for _, text in sorted(functions))]
    return '\n'.join(sections) + '\n'
