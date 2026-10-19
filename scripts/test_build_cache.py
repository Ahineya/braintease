#!/usr/bin/env python3
"""Cache invalidation tests without invoking compilers or opening user disks."""
import os
from pathlib import Path
import tempfile
import unittest
from build_cache import cached_step, c_dependencies


class CacheTests(unittest.TestCase):
    def test_content_outputs_commands_and_failure(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp);source=root/'in';output=root/'out';stamp=root/'cache.json'
            source.write_text('one');calls=[]
            def build():calls.append(1);output.write_text(source.read_text())
            def step(settings=(),force=False):
                return cached_step(stamp,[source],settings,build,[output],force)
            self.assertTrue(step());self.assertFalse(step())
            os.utime(source,None);self.assertFalse(step())
            old=source.stat();source.write_text('two');os.utime(source,ns=(old.st_atime_ns,old.st_mtime_ns))
            self.assertTrue(step());self.assertEqual(output.read_text(),'two')
            output.unlink();self.assertTrue(step())
            output.write_text('bad');self.assertTrue(step())
            self.assertTrue(step(['changed option']));self.assertTrue(step(['changed option'],True))
            self.assertTrue(step([source]));self.assertFalse(step([str(source)]))
            def fail():raise RuntimeError('compiler failed')
            with self.assertRaises(RuntimeError):cached_step(stamp,[source],['changed option'],fail,[output],True)
            self.assertFalse(stamp.exists());self.assertTrue(step())

    def test_transitive_implementation_includes(self):
        with tempfile.TemporaryDirectory() as temp:
            root=Path(temp);(root/'nested').mkdir()
            (root/'main.c').write_text('#include "nested/a.inc"\n#include <header.h>\n')
            (root/'nested/a.inc').write_text('#include "b.c"\n')
            (root/'nested/b.c').write_text('int n;\n')
            (root/'header.h').write_text('#include "header.h"\n')
            (root/'unrelated.c').write_text('int unrelated;\n')
            actual=c_dependencies(root/'main.c',[root])
            self.assertEqual({p.name for p in actual},{'main.c','a.inc','b.c','header.h'})


if __name__=='__main__':unittest.main()
