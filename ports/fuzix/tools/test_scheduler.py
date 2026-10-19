#!/usr/bin/env python3
"""Exercise real kernel queue code with deterministic host-side state traces."""
from pathlib import Path
import os
import subprocess
import tempfile
import unittest
ROOT=Path(__file__).resolve().parents[3]
class SchedulerTests(unittest.TestCase):
    def test_queues_states_wait_collisions_and_elapsed_budget(self):
        env=os.environ.copy()
        if Path('/Applications/Xcode.app/Contents/Developer').is_dir():
            env.setdefault('DEVELOPER_DIR','/Applications/Xcode.app/Contents/Developer')
        with tempfile.TemporaryDirectory() as directory:
            fixture=ROOT/'ports/fuzix/tools/scheduler-test'
            executable=Path(directory)/'scheduler'
            sdk=Path('/Applications/Xcode.app/Contents/Developer/Platforms/MacOSX.platform/Developer/SDKs/MacOSX.sdk')
            sysroot=['-isysroot',str(sdk)] if sdk.exists() else []
            for quantum in [20, 5]:
                subprocess.run(['cc',*sysroot,'-std=c99','-Wall','-Wextra','-Werror',
                    '-DRIPPLE_SCHED_QUANTUM_MS='+str(quantum),'-I'+str(fixture),
                    '-I'+str(ROOT/'ports/fuzix/upstream/Kernel/include'),str(fixture/'check.c'),'-o',str(executable)],check=True,env=env)
                subprocess.run([str(executable)],check=True,timeout=10)
if __name__=='__main__':unittest.main()
