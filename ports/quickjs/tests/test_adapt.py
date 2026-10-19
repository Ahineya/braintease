"""Protect guest source adaptation from changing language data or C ABIs."""
import sys
from pathlib import Path
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from adapt import private_ints, octet_names
from core_frames import transform_function
from core_heap import core_integer_constants
from core_libc import parser_formats, libc_formats, format_contract


class SourceAdaptation(unittest.TestCase):
    def test_complete_format_argument_contract(self):
        fmt, types = format_contract('%02d %0*d %.*s %c %u %p %lld %.3g %%')
        self.assertEqual(fmt, '%02ld %0*ld %.*s %c %lu %p %lld %.3g %%')
        self.assertEqual(types, ['long', 'int16_t', 'long', 'int16_t', 'const char *',
                                'int16_t', 'unsigned long', 'void *', 'long long', 'double'])

    def test_format_literals_and_macros_keep_exact_varargs_types(self):
        source = '''// printf("%d", untouched)
const char *js = "printf(\\\"%d\\\", value)";
snprintf(buf, sizeof(buf), "%d:%0*d %" PRIx64, 42, width, f(a,b), bits);
js_parse_error(s, "expecting '%c'", '}');
JS_ThrowTypeErrorAtom(ctx, "'%s'", atom);
JS_ThrowTypeError(ctx, fmt, pointer);
'''
        result = libc_formats(source)
        self.assertIn('"%ld:%0*ld %llx", (long)(42), (int16_t)(width), (long)(f(a,b)), (unsigned long long)(bits)', result)
        self.assertIn('"expecting \'%c\'", (int16_t)(\'}\')', result)
        self.assertIn('const char *js = "printf(\\\"%d\\\", value)";', result)
        self.assertIn('JS_ThrowTypeErrorAtom(ctx, "\'%s\'", atom);', result)
        self.assertIn('JS_ThrowTypeError(ctx, fmt, pointer);', result)

    def test_unknown_format_contracts_fail_generation(self):
        for source in ['printf("%d", x, y);', 'printf("%d");', 'printf("%n", p);',
                       'printf("%2$d", x);', 'printf("%" UNKNOWN, x);']:
            with self.subTest(source=source), self.assertRaises(ValueError):
                libc_formats(source)

    def test_parser_precision_keeps_libc_int_abi(self):
        source = '''snprintf(str, sizeof str, "%.*s",
                 (int)(s->buf_ptr - s->token.ptr - 2), s->token.ptr + 1);
js_parse_error(s, "%.*s", (int)(s->buf_ptr - s->token.ptr), s->token.ptr);
js_parse_error(s, "%.*s", (int)(s->buf_ptr - s->token.ptr), s->token.ptr);
'''
        result = private_ints(parser_formats(source))
        self.assertEqual(result.count('(int16_t)min_int('), 3)
        self.assertIn('sizeof(str) - 1', result)
        self.assertEqual(result.count('s->token.ptr, 255)'), 2)
        self.assertEqual(result.count('"%.*s"'), 3)

    def test_preserve_language_data(self):
        source = '''/* int unsigned uint8_t */
const char *s = "int unsigned uint8_t \\" int8_t";
// int uint8_t
int n; unsigned flags; uint8_t octet; int8_t signed_octet;
'''
        expected = '''/* int unsigned uint8_t */
const char *s = "int unsigned uint8_t \\" int8_t";
// int uint8_t
long n; unsigned long flags; qjs_octet_t octet; qjs_signed_octet_carrier_t signed_octet;
'''
        self.assertEqual(private_ints(source), expected)

    def test_compound_types_with_comments(self):
        self.assertEqual(private_ints('unsigned /* ABI */ long n; unsigned int i;'),
                         'unsigned /* ABI */ long n; unsigned long i;')
        with self.assertRaises(ValueError):
            private_ints('long /* compound spelling */ int n;')
        self.assertEqual(private_ints('const char *s = "long int"; int n;'),
                         'const char *s = "long int"; long n;')

    def test_preserve_headers_literals_and_names(self):
        source = '#include "int.h"\nint interrupt; qjs_c_int sdk_arg; uint32_t hint;'
        self.assertEqual(private_ints(source),
                         '#include "int.h"\nlong interrupt; qjs_c_int sdk_arg; uint32_t hint;')
        self.assertEqual(octet_names("'uint8_t' /* int8_t */ uint8_t octet;"),
                         "'uint8_t' /* int8_t */ qjs_octet_t octet;")

    def test_definition_rewrite_does_not_match_a_call_or_macro(self):
        source = '''int caller(void) { if (target()) { return 7; } return 8; }
#define CALL() target((int[]){1,2})
static int target(void) { const char *brace="}"; /* { */ return 0; }
'''
        result = transform_function(source, 'target', lambda body: body.replace('return 0;', 'return 42;'))
        self.assertEqual(result, source.replace('return 0;', 'return 42;'))

    def test_shift_width_changes_preserve_javascript_and_comments(self):
        source = '''const char *s = "1 << 30"; /* 1U << 31 */
int value = 1U << 31;
rt->malloc_gc_threshold = 256 * 1024;
#define JS_BIGINT_MAX_SIZE ((1024 * 1024) / JS_LIMB_BITS)
'''
        result = core_integer_constants(source)
        self.assertIn('"1 << 30"; /* 1U << 31 */', result)
        self.assertIn('value = 1UL << 31;', result)


if __name__ == '__main__':
    unittest.main()
