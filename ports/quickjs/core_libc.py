"""Explicit libc ABI boundaries inside the widened private engine.

Do not widen libc's varargs contract together with QuickJS's private integers.
These parser precision arguments consume an ordinary int on each host: a
16-bit cast promotes to int on ordinary hosts and is already int-sized on C16.
"""
import ast
import json
import re
from adapt import replace, _C_TOKENS


def parser_formats(source):
    source = replace(source, '(int)(s->buf_ptr - s->token.ptr - 2), s->token.ptr + 1);',
                     '(int16_t)min_int(s->buf_ptr - s->token.ptr - 2, sizeof(str) - 1), s->token.ptr + 1);')
    # The error formatter has a 256-cell destination. Bound precision before
    # narrowing so long tokens cannot wrap to a negative/unlimited precision.
    return replace(source, '(int)(s->buf_ptr - s->token.ptr), s->token.ptr);',
                   '(int16_t)min_int(s->buf_ptr - s->token.ptr, 255), s->token.ptr);', 2)


# Format argument positions, excluding the atom macros: their %s argument is
# an atom ID, expanded to a string by a separate, typed helper.
_FORMATS = dict(printf=0, fprintf=1, snprintf=2, dbuf_printf=1, js_printf=1,
                bc_read_trace=1, js_parse_error=1, js_parse_error_pos=2,
                JS_ThrowSyntaxError=1, JS_ThrowTypeError=1, JS_ThrowRangeError=1,
                JS_ThrowReferenceError=1, JS_ThrowInternalError=1,
                JS_ThrowTypeErrorOrFalse=2, js_throw_URIError=1)
_CONVERSION = re.compile(r'%([-+ #0]*)(\*|\d+)?(\.\*|\.\d*)?(hh|ll|[hljztL])?([diuoxXcsp%fFeEgGaA])')


def _format_literal(tokens):
    result = ''
    for token in tokens:
        word = token[0]
        if word.startswith('"'):
            result += ast.literal_eval(word)
        elif word == 'CONFIG_VERSION':
            result += '2026-06-04'  # pinned, never read from untrusted input
        else:
            macro = re.fullmatch(r'PRI([diuoxX])(16|32|64|PTR)', word)
            if not macro:
                raise ValueError('Unsupported format expression token: ' + word)
            result += {'16': '', '32': 'l', '64': 'll', 'PTR': 'l'}[macro[2]] + macro[1]
    return result


def format_contract(fmt):
    """Return a libc format and one exact promoted type per consumed argument."""
    result, types, end = [], [], 0
    while '%' in fmt[end:]:
        start = fmt.index('%', end)
        match = _CONVERSION.match(fmt, start)
        if not match:
            raise ValueError('Unsupported printf conversion: ' + fmt[start:])
        flags, width, precision, length, kind = match.groups()
        result.append(fmt[end:start])
        length = length or ''
        if kind == '%':
            if match[0] != '%%': raise ValueError('Nonliteral percent conversion')
            result.append('%%'); end = match.end(); continue
        if width == '*': types.append('int16_t')
        if precision == '.*': types.append('int16_t')
        if kind in 'diuoxX':
            if length in ('h', 'hh', 'L'):
                raise ValueError('Unaudited narrow/long-double format: ' + match[0])
            length = 'll' if length in ('ll', 'j') else 'l'
            ctype = 'long long' if length == 'll' else 'long'
            if kind in 'uoxX': ctype = 'unsigned ' + ctype
        elif kind == 'c':
            if length: raise ValueError('Wide character format is unsupported')
            ctype = 'int16_t'
        elif kind == 's':
            if length: raise ValueError('Wide string format is unsupported')
            ctype = 'const char *'
        elif kind == 'p':
            if length: raise ValueError('Length modifier on pointer format')
            ctype = 'void *'
        else:
            if length not in ('', 'l'): raise ValueError('Long double format is unsupported')
            length, ctype = '', 'double'
        types.append(ctype)
        result.append('%' + flags + (width or '') + (precision or '') + length + kind)
        end = match.end()
    return ''.join(result) + fmt[end:], types


def libc_formats(source):
    """Rewrite literal printf call contracts, preserving forwarding va_lists.

    Cast every argument, including literals/enums, to its format's actual ABI.
    Rewriting %d to %ld alone would still misread a one-cell enum/literal.
    Token boundaries preserve comments, JavaScript data, and nested expressions.
    Unknown format syntax or mismatched argument counts fail generation.
    """
    tokens = [t for t in _C_TOKENS.finditer(source)
              if not t[0].startswith(('/*', '//', '\\\n', '\\\r\n'))]
    edits = []
    for i, token in enumerate(tokens[:-1]):
        if token[0] not in _FORMATS or tokens[i + 1][0] != '(':
            continue
        args, begin, depth = [], i + 2, 0
        for j in range(begin, len(tokens)):
            word = tokens[j][0]
            if word == ')' and depth == 0:
                args.append(tokens[begin:j]); break
            if word == ',' and depth == 0:
                args.append(tokens[begin:j]); begin = j + 1
            elif word in ('(', '[', '{'): depth += 1
            elif word in (')', ']', '}'): depth -= 1
        else:
            raise ValueError('Unclosed printf call')
        index = _FORMATS[token[0]]
        if len(args) <= index or not args[index] or not args[index][0][0].startswith('"'):
            continue  # definition, macro parameter, or typed va_list forwarding
        fmt, types = format_contract(_format_literal(args[index]))
        values = args[index + 1:]
        if len(types) != len(values):
            raise ValueError(f'{token[0]} format consumes {len(types)} args, supplied {len(values)}: {fmt}')
        edits.append((args[index][0].start(), args[index][-1].end(), json.dumps(fmt)))
        for value, ctype in zip(values, types):
            start, end = value[0].start(), value[-1].end()
            edits.append((start, end, f'({ctype})({source[start:end]})'))
    edits.sort()
    for previous, following in zip(edits, edits[1:]):
        if previous[1] > following[0]:
            raise ValueError('Nested printf calls require an explicit adaptation')
    for start, end, text in reversed(edits):
        source = source[:start] + text + source[end:]
    return source
