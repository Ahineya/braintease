"""Production arena layout and checked allocation arithmetic for C16."""
import re
from adapt import replace, _C_TOKENS
from core_frames import transform_function


def arena_storage(source):
    # C16 fields before ref_count occupy three cells rather than four host
    # octets. Pad BEFORE the reference count: public JS_DupValue expects the
    # ref_count immediately before the payload. Eight-cell headers also
    # preserve pointer-tag alignment through every size class.
    source = replace(source, '    int ref_count;\n    __attribute__((aligned(JS_MALLOC_ALIGN))) uint8_t user_data[];', '''#if CHAR_BIT == 16
    uint8_t c16_padding[3];
#endif
    int ref_count;
    __attribute__((aligned(JS_MALLOC_ALIGN))) uint8_t user_data[];''')
    # Keep the alignment mask explicitly in the allocation-size domain.
    source = replace(source, '& ~(JS_MALLOC_ALIGN - 1)', '& ~(size_t)(JS_MALLOC_ALIGN - 1)', 2)
    for name in ('__js_malloc', '__js_realloc'):
        source = transform_function(source, name, lambda body: '''
    if (size > SIZE_MAX - sizeof(JSMallocBlockHeader) - (JS_MALLOC_ALIGN - 1))
        return NULL;
''' + body)
    source = transform_function(source, 'js_malloc_large', lambda body: '''
    if (size > SIZE_MAX - sizeof(JSMallocLargeBlockHeader)) return NULL;
''' + body)
    return source


def atom_storage(source):
    for before, after in [
        ('#define JS_ATOM_HASH_MASK  ((1 << 30) - 1)', '#define JS_ATOM_HASH_MASK  ((1UL << 30) - 1)'),
        ('#define JS_ATOM_TAG_INT (1U << 31)', '#define JS_ATOM_TAG_INT (1UL << 31)'),
        ('#define JS_ATOM_MAX     ((1U << 30) - 1)', '#define JS_ATOM_MAX     ((1UL << 30) - 1)'),
    ]:
        source = replace(source, before, after)
    return source


def closure_storage(source):
    # GCC uses an unsigned representation for this nonnegative enum. RCC's
    # enum is signed: a three-bit field would sign-extend kinds 4..7 to -4..-1,
    # breaking global lexical closures. Use an explicit unsigned storage unit.
    return replace(source, '    JSClosureTypeEnum closure_type : 3;',
                   '    uint32_t closure_type : 3;')


def core_integer_constants(source):
    # Upstream's private integers are 32 bits. Widen decimal shift operands
    # in code only; comments, strings, character literals and include paths
    # are not C arithmetic. Enums and SDK declarations remain untouched.
    protected = [(t.start(), t.end()) for t in _C_TOKENS.finditer(source)
                 if t[0].startswith(('/*', '//', '"', "'"))]
    changes = []
    for m in re.finditer(r'\b([1-9][0-9]*)([uU]?)(\s*<<)', source):
        if not any(a <= m.start() < b for a, b in protected):
            changes.append((m.start(), m.end(), m[1] + ('UL' if m[2] else 'L') + m[3]))
    for a, b, text in reversed(changes):
        source = source[:a] + text + source[b:]
    source = replace(source, 'rt->malloc_gc_threshold = 256 * 1024;',
                     'rt->malloc_gc_threshold = 256UL * 1024;')
    source = replace(source, '#define JS_BIGINT_MAX_SIZE ((1024 * 1024) / JS_LIMB_BITS)',
                     '#define JS_BIGINT_MAX_SIZE ((1024UL * 1024) / JS_LIMB_BITS)')
    return source
