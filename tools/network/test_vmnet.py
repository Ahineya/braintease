#!/usr/bin/env python3
"""Exercise the actual macOS helper loop with fake vmnet and transient I/O faults."""
import os
from pathlib import Path
import socket
import subprocess
import sys
import tempfile
import time
import unittest

HERE = Path(__file__).resolve().parent


@unittest.skipUnless(sys.platform == 'darwin', 'macOS vmnet headers required')
class HelperTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.build = tempfile.TemporaryDirectory(prefix='vmnet-test-', dir='/tmp')
        cls.executable = Path(cls.build.name) / 'helper'
        sdk = subprocess.check_output(['xcrun', '--sdk', 'macosx', '--show-sdk-path'], text=True).strip()
        subprocess.run(['xcrun', 'clang', '-std=c11', '-O2', '-Wall', '-Wextra',
                        '-Werror', '-fblocks', '-isysroot', sdk, str(HERE / 'vmnet_test.c'),
                        '-framework', 'vmnet', '-o', str(cls.executable)], check=True)

    @classmethod
    def tearDownClass(cls):
        cls.build.cleanup()

    def run_peer(self, settings, exercise, expected_status=0, managed=True):
        with tempfile.TemporaryDirectory(prefix='vmnet-peer-', dir='/tmp') as directory:
            path = Path(directory)
            with (path / 'log').open('w+b') as log, socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM) as client:
                client.bind(str(path / 'client'))
                client.settimeout(2)
                process = subprocess.Popen([str(self.executable), str(path / 'nic.sock'), 'shared',
                                            *(['--managed'] if managed else [])],
                                           stdout=subprocess.DEVNULL, stderr=log,
                                           env=dict(os.environ, **settings))
                try:
                    deadline = time.monotonic() + 3
                    while not (path / 'nic.sock').exists():
                        self.assertIsNone(process.poll())
                        self.assertLess(time.monotonic(), deadline)
                        time.sleep(.005)
                    client.connect(str(path / 'nic.sock'))
                    client.send(b'RNET\1')
                    self.assertEqual(client.recv(64), b'RNET\1\2\3\4\5\6\7\5\xdc')
                    exercise(client, process)
                    if not expected_status:
                        self.assertIsNone(process.poll(), 'transient congestion killed the helper')
                finally:
                    if process.poll() is None:
                        process.terminate()
                    process.wait(timeout=5)
                    log.seek(0)
                    self.output = log.read().decode()
                self.assertEqual(process.returncode, expected_status, self.output)
                self.assertFalse((path / 'nic.sock').exists(), self.output)

    def test_control_backpressure_preserves_peer(self):
        for fault in ['eagain', 'enobufs', 'eintr']:
            with self.subTest(fault=fault):
                def exercise(client, process):
                    client.send(b'RNET\2')
                    # Either a pending reply is retried or this next heartbeat
                    # succeeds. Congestion must never revoke the attachment.
                    time.sleep(.02)
                    client.send(b'RNET\2')
                    self.assertEqual(client.recv(64), b'RNET\2')
                self.run_peer({'TEST_CONTROL_FAULT': fault}, exercise)

    def test_vmnet_buffer_exhaustion_retries_without_losing_order(self):
        def exercise(client, process):
            for value in [42, 43]:
                client.send(bytes([0] * 14 + [value] * 46))
            time.sleep(.1)
            client.send(b'RNET\2')
            self.assertEqual(client.recv(64), b'RNET\2')
        for busy in ['buffer', 'zero']:
            with self.subTest(busy=busy):
                self.run_peer({'TEST_VMNET_BUSY': busy, 'TEST_READ_BUSY': '1'}, exercise)
                self.assertIn('TEST accepted 60 42', self.output)
                self.assertIn('TEST accepted 60 43', self.output)
                self.assertLess(self.output.index('TEST accepted 60 42'), self.output.index('TEST accepted 60 43'))

    def test_hello_backpressure_is_retried(self):
        for fault in ['eagain', 'enobufs', 'eintr']:
            with self.subTest(fault=fault):
                def exercise(client, process):
                    client.send(b'RNET\2')
                    self.assertEqual(client.recv(64), b'RNET\2')
                self.run_peer({'TEST_CONTROL_FAULT': fault, 'TEST_HELLO_FAULT': '1'}, exercise)

    def test_persistent_vmnet_stall_reports_failure_while_servicing_heartbeats(self):
        def exercise(client, process):
            client.send(bytes([0] * 60))
            deadline = time.monotonic() + 7
            replies = 0
            while True:
                self.assertLess(time.monotonic(), deadline)
                try:
                    client.send(b'RNET\2')
                except ConnectionResetError:
                    # Darwin can report the peer's exit on send even though
                    # its last link-failure datagram is already queued for us.
                    pass
                reply = client.recv(64)
                if reply == b'RNET\3':
                    break
                self.assertEqual(reply, b'RNET\2')
                replies += 1
                time.sleep(.1)
            self.assertGreater(replies, 10)
            self.assertEqual(process.wait(timeout=3), 1)
        self.run_peer({'TEST_VMNET_BUSY': 'forever'}, exercise, expected_status=1)
        self.assertIn('vmnet_write stalled for 5 seconds (status=1007 count=0)', self.output)

    def test_fatal_vmnet_failure_reports_link_down_and_actual_status(self):
        def exercise(client, process):
            client.send(bytes([0] * 60))
            self.assertEqual(client.recv(64), b'RNET\3')
            self.assertEqual(process.wait(timeout=3), 1)
        self.run_peer({'TEST_VMNET_FATAL': '1'}, exercise, expected_status=1)
        self.assertIn('vmnet_write failed (status=1001 count=1)', self.output)

    def test_detach_discards_old_peers_queued_frames(self):
        def exercise(client, process):
            client.send(bytes([0] * 14 + [42] * 46))
            client.send(b'RNET\4')
            time.sleep(.02)
            client.send(b'RNET\1')
            self.assertTrue(client.recv(64).startswith(b'RNET\1'))
            client.send(bytes([0] * 14 + [43] * 46))
            time.sleep(.03)
            client.send(b'RNET\2')
            self.assertEqual(client.recv(64), b'RNET\2')
        self.run_peer({'TEST_VMNET_BUSY': 'buffer'}, exercise, managed=False)
        self.assertNotIn('TEST accepted 60 42', self.output)
        self.assertIn('TEST accepted 60 43', self.output)


if __name__ == '__main__':
    unittest.main()
