import sys
import unittest
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from adapt import octet_casts


class OctetCastTests(unittest.TestCase):
    def test_unary_boundaries_and_nested_casts(self):
        for original, expected in [
            ('(byte)x + y', 'WC_OCTET(x) + y'),
            ('(byte)(x+y) * z', 'WC_OCTET((x+y)) * z'),
            ('(byte)p->data[i++] + 1', 'WC_OCTET(p->data[i++]) + 1'),
            ('(byte)~(1U << n)', 'WC_OCTET(~(1U << n))'),
            ('(byte)(byte)x', 'WC_OCTET(WC_OCTET(x))'),
            ('(byte)(word32)x + y', 'WC_OCTET((word32)x) + y'),
            ('(byte)f(a,b)', 'WC_OCTET(f(a,b))'),
            ('(byte)*p++', 'WC_OCTET(*p++)'),
        ]:
            actual, count = octet_casts(original)
            self.assertEqual(actual, expected)
            self.assertGreater(count, 0)

    def test_pointer_casts_comments_strings_and_continuations(self):
        original = '(byte*)p; /* (byte)x */ "(byte)x"; (byte) \\\n x + 1'
        actual, count = octet_casts(original)
        self.assertEqual(actual, original.replace('(byte) \\\n x', 'WC_OCTET( \\\n x)'))
        self.assertEqual(count, 1)


if __name__ == '__main__':
    unittest.main()
