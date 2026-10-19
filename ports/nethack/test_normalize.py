#!/usr/bin/env python3
import unittest
from normalize import normalize, cast_opvars

class TranslationTests(unittest.TestCase):
    def test_struct_tag_and_shared_type(self):
        source = 'void\nchoose(list, obj)\nstruct obj *list, *obj;\n{ return; }'
        self.assertEqual(normalize(source),'void\nchoose(struct obj *list, struct obj *obj)\n{ return; }')
    def test_function_pointer_and_spacing(self):
        source = 'static int hit (target, callback )\nstruct obj *target;\nint (*callback)(struct obj *);\n{ return callback(target); }'
        self.assertIn('hit(struct obj *target, int (*callback)(struct obj *))', normalize(source))
    def test_flow_control_is_unchanged(self):
        source = 'if (active)\n  draw();\nif (other) { update(); }'
        self.assertEqual(normalize(source),source)
    def test_variadic_cell_widths(self):
        source = 'add_opvars(level, "ilso", position.x, 0, lookup(a,b), SPO_EXIT);'
        self.assertEqual(cast_opvars(source),'add_opvars(level, "ilso", (int)(position.x), (long)(0), (const char *)(lookup(a,b)), (int)(SPO_EXIT));')
    def test_nonliteral_prototype(self):
        source = 'void add_opvars(sp_lev *level, const char *format, ...);'
        self.assertEqual(cast_opvars(source),source)

if __name__ == '__main__': unittest.main()
