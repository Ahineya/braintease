"""Exercise the USB command gate without opening a device or linking IOKit."""
import pathlib
import shutil
import subprocess
import tempfile
import unittest


class USBPolicyTests(unittest.TestCase):
    def test_only_published_queries_and_bounded_controls(self):
        compiler = shutil.which("clang") or shutil.which("cc")
        if not compiler:
            self.skipTest("C compiler unavailable")
        source = r'''
#include <assert.h>
#include <string.h>
#include "usb_policy.h"
int main(void) {
    uint8_t b[65] = {0x80};
    unsigned allowed = 0;
    for (unsigned group = 0; group < 256; ++group)
        for (unsigned command = 0; command < 256; ++command) {
            b[1] = group; b[2] = command;
            bool expected = (group == 3 && command == 3) ||
                (group == 12 && command == 2) || (group == 5 && (command == 2 || command == 4)) ||
                (group == 1 && (command == 9 || command == 17)) ||
                (group == 0x70 && command == 1);
            assert(ds_send_allowed(2, b, 64) == expected);
            allowed += expected;
        }
    assert(allowed == 7);
    b[1] = 0x70; b[2] = 1;
    for (size_t n = 0; n <= 65; ++n) assert(ds_send_allowed(2, b, n) == (n == 64));
    b[1] = 3; b[2] = 3;
    for (size_t n = 0; n <= 65; ++n)
        assert(ds_send_allowed(2, b, n) == (n >= 3 && n <= 64));
    for (size_t i = 3; i < 64; ++i) {
        b[i] = 1; assert(!ds_send_allowed(2, b, 64)); b[i] = 0;
    }
    for (unsigned id = 0; id < 256; ++id) {
        b[0] = id;
        assert(ds_send_allowed(2, b, 64) == (id == 0x80));
        for (size_t n = 0; n <= 65; ++n)
            assert(ds_get_allowed(id, n) == ((n >= 1 && n <= 64 && (id == 5 || id == 0x20 || id == 0x81)) ||
                                           (n == 42 && (id == 0x0b || id == 0x0c))));
    }
    assert(!ds_send_allowed(2, NULL, 64));
    /* Loader/update/persistence commands stay blocked in both directions. */
    const uint8_t forbidden[] = {0xa0,0xf4,0xf5,0xf6,0xf7};
    for (size_t i = 0; i < sizeof(forbidden); ++i) {
        memset(b, 0, sizeof(b)); b[0] = forbidden[i];
        assert(!ds_send_allowed(2, b, 64));
        for (size_t n = 0; n <= 65; ++n) assert(!ds_get_allowed(forbidden[i], n));
    }
    b[0] = 0x80; b[1] = 3; b[2] = 2;
    assert(!ds_send_allowed(2, b, 64));
    memset(b, 0, sizeof(b)); b[0] = 2; b[2] = 4;
    assert(ds_send_allowed(1, b, 48));
    for (size_t i = 3; i < 48; ++i) {
        for (unsigned val = 1; val < 256; ++val) {
            b[i] = val;
            assert(ds_send_allowed(1, b, 48) == (i >= 45 && val <= 16));
        }
        b[i] = 0;
    }
    b[1] = 3; b[2] = 0;
    assert(ds_send_allowed(1, b, 48));
    for (size_t i = 3; i < 48; ++i) {
        for (unsigned val = 1; val < 256; ++val) {
            b[i] = val;
            assert(ds_send_allowed(1, b, 48) == (i <= 4 && val <= 24));
        }
        b[i] = 0;
    }
    for (size_t n = 0; n <= 65; ++n) assert(ds_send_allowed(1, b, n) == (n == 48));
    assert(!ds_send_allowed(0, b, 48));
    assert(!ds_send_allowed(3, b, 48));
    return 0;
}
'''
        with tempfile.TemporaryDirectory() as directory:
            cfile = pathlib.Path(directory) / "policy.c"
            binary = pathlib.Path(directory) / "policy"
            cfile.write_text(source)
            args = [compiler, "-std=c11", "-Wall", "-Wextra", "-Werror"]
            sdk = pathlib.Path("/Applications/Xcode.app/Contents/Developer/Platforms/MacOSX.platform/Developer/SDKs/MacOSX15.5.sdk")
            if sdk.is_dir():
                args += ["-isysroot", str(sdk)]
            args += ["-I", str(pathlib.Path(__file__).resolve().parent), str(cfile), "-o", str(binary)]
            subprocess.run(args, check=True, capture_output=True, timeout=30)
            subprocess.run([str(binary)], check=True, capture_output=True, timeout=10)


if __name__ == "__main__":
    unittest.main()
