"""Logical-octet branches, compact immediates and optimizer pattern arguments.

These changes are portable and are tested on the full host engine as well as
isolated C16 production routines. This is not a dispatch/object-layout audit.
"""
from adapt import replace
from core_frames import transform_function


def bytecode_operands(source):
    # emit_op deliberately defers allocation failure in DynBuf. Preserve that
    # failure before resolve_variables replaces the input with a fresh buffer;
    # otherwise low-memory compilation analyzes a truncated instruction stream
    # and misreports an internal bytecode/stack error instead of OOM.
    source = transform_function(source, 'js_create_function', lambda body: replace(
        body, '    /* recompute scope linkage */', '''    if (dbuf_error(&fd->byte_code)) {
        JS_ThrowOutOfMemory(ctx);
        goto fail;
    }

    /* recompute scope linkage */'''))
    for before, after, count in [
        ('(int8_t)pc[0]', 'get_i8(pc)', 1),
        ('(int8_t)pc[-1]', 'get_i8(pc - 1)', 2),
        ('(int8_t)tab[pos]', 'get_i8(tab + pos)', 1),
        ('(int8_t)bc_buf[pos + 1]', 'get_i8(bc_buf + pos + 1)', 2),
        ('val == (int8_t)val', '(val >= -128 && val <= 127)', 1),
        ('diff == (int8_t)diff', '(diff >= -128 && diff <= 127)', 2),
    ]:
        source = replace(source, before, after, count)
    # The C16 unsigned octet carrier otherwise promotes -3 to unsigned
    # 16-bit before the widened assignment, turning a negative stack offset
    # into 65533 and reading outside the interpreter's iterator record.
    source = replace(source, 'int offset = -3 - pc[0];',
                     'int offset = -3L - (int32_t)pc[0];')
    # All callers supply an inline pattern terminated with -1. C16 enum and
    # integer literal varargs otherwise occupy one cell, but the widened core
    # retrieves two-cell ints. A typed compound literal handles conversions
    # at the call site and avoids a private varargs ABI altogether.
    def pattern_arguments(body):
        body = replace(body, '    va_list ap;', '')
        body = replace(body, '    va_start(ap, pos);', '')
        body = replace(body, 'va_arg(ap, int)', '*pattern++', 3)
        return replace(body, '    va_end(ap);', '')
    source = transform_function(source, 'code_match', pattern_arguments)
    source = replace(source, 'static BOOL code_match(CodeContext *s, int pos, ...)', '''#define code_match(s, pos, ...) \\
    qjs_code_match((s), (pos), (const int32_t[]){ __VA_ARGS__ })
static BOOL qjs_code_match(CodeContext *s, int pos, const int32_t *pattern)''')
    for before, after in [
        ('(uint8_t)op1', '((uint32_t)op1 & 255U)'),
        ('(uint8_t)(op1 >> 8)', '(((uint32_t)op1 >> 8) & 255U)'),
        ('(uint8_t)(op1 >> 16)', '(((uint32_t)op1 >> 16) & 255U)'),
        ('(uint8_t)(op1 >> 24)', '(((uint32_t)op1 >> 24) & 255U)'),
    ]:
        source = replace(source, before, after, 2 if before == '(uint8_t)op1' else 1)
    for n in (2, 3, 4):
        before = f'#define M{n}(' + ', '.join(f'op{i}' for i in range(1, n + 1)) + ')'
        line = next(line for line in source.splitlines() if line.startswith(before))
        value = ' | '.join(f'((uint32_t)(op{i}) << {8 * (i - 1)})' for i in range(1, n + 1))
        source = replace(source, line, before + ' (' + value + ')')
    return source
