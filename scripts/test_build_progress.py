#!/usr/bin/env python3
"""Check live build output, quiet-step heartbeats, errors and interruption."""
import io
import os
from pathlib import Path
import signal
import subprocess
import sys
import tempfile
import time
import unittest
from build_progress import watch


class ProgressTests(unittest.TestCase):
    def test_activity_and_heartbeat_keep_full_log(self):
        with tempfile.TemporaryDirectory() as temp:
            log = Path(temp) / 'build.log'
            output = io.StringIO()
            code = ('import time; print("Build: Building kernel", flush=True); '
                    'print("  RCC ripple/console.c", flush=True); '
                    'print("private compiler detail", flush=True); time.sleep(.2); '
                    'print("fuzix: updated", flush=True)')
            self.assertEqual(watch([sys.executable, '-c', code], log, output,
                                   heartbeat=.04, activity_interval=0), 0)
            terminal = output.getvalue()
            self.assertIn('RCC ripple/console.c', terminal)
            self.assertIn('Waiting for build output;', terminal)
            self.assertIn('fuzix: updated', terminal)
            self.assertIn('Build complete', terminal)
            self.assertNotIn('private compiler detail', terminal)
            self.assertIn('private compiler detail', log.read_text())

    def test_uninteresting_output_does_not_hide_heartbeat(self):
        with tempfile.TemporaryDirectory() as temp:
            log = Path(temp) / 'build.log'
            output = io.StringIO()
            code = ('import time\nprint("Build: Linking kernel", flush=True)\n'
                    'for i in range(20):\n print("diagnostic detail", flush=True); time.sleep(.01)\n')
            self.assertEqual(watch([sys.executable, '-c', code], log, output, heartbeat=.04), 0)
            self.assertIn('last update: Linking kernel', output.getvalue())
            self.assertNotIn('diagnostic detail', output.getvalue())
            self.assertEqual(log.read_text().count('diagnostic detail'), 20)

    def test_heartbeat_tracks_pending_tasks_instead_of_last_completed_app(self):
        with tempfile.TemporaryDirectory() as temp:
            output = io.StringIO()
            code = ('import time\n'
                    'print("Build task started: lynx", flush=True)\n'
                    'print("Build task started: yes", flush=True)\n'
                    'print("Build task progress: lynx: Build: Linking native functions", flush=True)\n'
                    'print("Build task finished: yes", flush=True)\n'
                    'print("yes: built", flush=True)\n'
                    'time.sleep(.15)\n'
                    'print("Build task finished: lynx", flush=True)\n')
            self.assertEqual(watch([sys.executable, '-c', code], Path(temp) / 'log',
                                   output, heartbeat=.03, activity_interval=0), 0)
            beats = [line for line in output.getvalue().splitlines() if 'Still working:' in line]
            self.assertTrue(beats)
            self.assertTrue(all('lynx: Linking native functions' in line for line in beats))
            self.assertTrue(all('yes' not in line for line in beats))

    def test_failure_preserves_exit_status_and_reports_diagnostics(self):
        with tempfile.TemporaryDirectory() as temp:
            log = Path(temp) / 'build.log'
            output = io.StringIO()
            code = 'import sys; print("syntax problem", file=sys.stderr); sys.exit(17)'
            self.assertEqual(watch([sys.executable, '-c', code], log, output), 17)
            self.assertIn('syntax problem', log.read_text())
            self.assertIn('syntax problem', output.getvalue())
            self.assertIn('Build failed (exit 17)', output.getvalue())
            self.assertIn(str(log), output.getvalue())

    def test_parallel_output_does_not_bury_the_original_failure(self):
        with tempfile.TemporaryDirectory() as temp:
            log = Path(temp) / 'build.log'
            output = io.StringIO()
            code = ('import sys; print("Application link failed: multiple definitions of function hypot"); '
                    'print("make: *** [graphics] Error 1"); '
                    '[print("RCC unrelated%d.c" % i) for i in range(100)]; sys.exit(2)')
            self.assertEqual(watch([sys.executable, '-c', code], log, output), 2)
            terminal = output.getvalue()
            self.assertIn('multiple definitions of function hypot', terminal.split('Build errors:')[0])
            self.assertIn('multiple definitions of function hypot', terminal.split('Build errors:')[1])
            self.assertIn('make: *** [graphics] Error 1', terminal)
            self.assertIn('RCC unrelated99.c', log.read_text())

    def test_interrupt_stops_the_owned_build_process(self):
        with tempfile.TemporaryDirectory() as temp:
            pidfile = Path(temp) / 'pid'
            code = ('import os,time; from pathlib import Path; '
                    f'Path({str(pidfile)!r}).write_text(str(os.getpid())); time.sleep(60)')
            process = subprocess.Popen([sys.executable, str(Path(__file__).with_name('build_progress.py')),
                                        '--log', str(Path(temp) / 'build.log'), '--',
                                        sys.executable, '-c', code],
                                       stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
            try:
                deadline = time.monotonic() + 5
                while not pidfile.exists() and time.monotonic() < deadline:
                    time.sleep(.01)
                self.assertTrue(pidfile.exists(), 'build child never started')
                child = int(pidfile.read_text())
                process.send_signal(signal.SIGINT)
                stdout, stderr = process.communicate(timeout=8)
                self.assertEqual(process.returncode, 130, stderr)
                self.assertIn('Build interrupted', stderr)
                self.assertEqual(stdout, '')
                with self.assertRaises(ProcessLookupError):
                    os.kill(child, 0)
            finally:
                if process.poll() is None:
                    process.kill()
                    process.communicate()


if __name__ == '__main__':
    unittest.main()
