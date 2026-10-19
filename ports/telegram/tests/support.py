"""Independent Python serialization, OpenSSL crypto oracle and host test build."""
import base64
import ctypes as C
import hashlib
import os
from pathlib import Path
import shlex
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[3]
PORT = ROOT / 'ports/telegram'
BUILD = ROOT / 'build/telegram-tests'


def tl(value):
    head = bytes([len(value)]) if len(value) < 254 else b'\xfe' + len(value).to_bytes(3, 'little')
    value = head + value
    return value + b'\0' * (-len(value) % 4)


def u32(value):
    return (value & 0xffffffff).to_bytes(4, 'little')


def u64(value):
    return value.to_bytes(8, 'little')


class Reader:
    def __init__(self, data):
        self.data, self.at = data, 0

    def take(self, n):
        assert 0 <= n <= len(self.data) - self.at
        value = self.data[self.at:self.at+n]
        self.at += n
        return value

    def integer(self, n=4):
        return int.from_bytes(self.take(n), 'little')

    def blob(self):
        n, head = self.integer(1), 1
        if n == 254:
            n, head = self.integer(3), 4
        assert n <= 16384
        value = self.take(n)
        assert self.take(-(n+head) % 4) == b'\0' * (-(n+head) % 4)
        return value

    def done(self):
        assert self.at == len(self.data), (self.at, len(self.data))


def der(data):
    tag, n, at = data[0], data[1], 2
    if n & 128:
        count = n & 127
        n, at = int.from_bytes(data[2:2+count], 'big'), 2+count
    return tag, data[at:at+n], data[at+n:]


def build():
    BUILD.mkdir(parents=True, exist_ok=True)
    key = BUILD / 'fixture.pem'
    if not key.exists():
        subprocess.run(['openssl', 'genrsa', '-traditional', '-out', str(key), '2048'], check=True, capture_output=True)
        key.chmod(0o600)
    encoded = base64.b64decode(''.join(key.read_text().splitlines()[1:-1]))
    _, data, _ = der(encoded)
    integers = []
    while data:
        _, value, data = der(data)
        integers.append(int.from_bytes(value, 'big'))
    _, n, e, d = integers[:4]
    modulus = n.to_bytes(256, 'big')
    fingerprint = hashlib.sha1(tl(modulus) + tl(e.to_bytes(3, 'big'))).digest()[12:]
    def array(value):
        return '{' + ','.join(map(str, value)) + '}'
    anchors = BUILD / 'fixture_keys.c'
    anchors.write_text('#include "core.h"\nconst TgByte tg_rsa_n[2][256]={' + array(modulus) + ',' + array(modulus) + '};\n' +
                       'const TgByte tg_rsa_fingerprint[2][8]={' + array(fingerprint) + ',' + array(fingerprint) + '};\n')
    compiler = ['cc']
    sdk = []
    if sys.platform == 'darwin':
        compiler = [subprocess.check_output(['xcrun', '--find', 'clang'], text=True).strip()]
        sdk = ['-isysroot', os.environ.get('SDKROOT') or subprocess.check_output(['xcrun', '--sdk', 'macosx', '--show-sdk-path'], text=True).strip()]
    openssl = shlex.split(subprocess.check_output(['pkg-config', '--cflags', '--libs', 'openssl'], text=True))
    units = [PORT / (name + '.c') for name in ('wire', 'hash', 'inflate', 'tg_crypto', 'packet', 'keys', 'storage', 'client', 'handshake', 'account', 'receive', 'tl', 'tl_schema', 'model', 'dialogs', 'updates')]
    library = BUILD / ('oracle.dylib' if sys.platform == 'darwin' else 'oracle.so')
    subprocess.run([*compiler, *sdk, '-std=c99', '-Wall', '-Wextra', '-Werror', '-Wno-deprecated-declarations',
                    '-DTG_TESTING', '-DTG_TEST_KEYS', '-fPIC', '-shared', '-idirafter', str(ROOT / 'runtime/include'),
                    '-I', str(PORT), *map(str, units), str(PORT / 'tests/crypto_shim.c'), str(ROOT / 'libraries/unicode/unicode.c'),
                    str(ROOT / 'rfl/os/socket.c'), str(ROOT / 'rfl/os/poll.c'), str(anchors), *openssl, '-o', str(library)], check=True)
    lib = C.CDLL(str(library))
    lib.rfl_crypto_call.argtypes = [C.c_uint, C.c_uint, C.c_void_p, C.c_ulong, C.c_void_p, C.c_ulong, C.c_void_p, C.c_ulong, C.c_void_p, C.c_ulong]
    lib.rfl_crypto_call.restype = C.c_long
    lib.tg_hash.argtypes = [C.c_int, C.c_void_p, C.c_uint, C.c_void_p]
    lib.tg_gunzip.argtypes = [C.c_void_p, C.c_uint, C.c_void_p, C.c_uint]
    lib.tg_group.argtypes = [C.c_void_p, C.c_uint]
    lib.tg_test_safe_prime.argtypes = [C.c_void_p]
    lib.tg_srp.argtypes = [C.c_void_p, C.c_uint, C.c_void_p, C.c_uint, C.c_void_p, C.c_uint, C.c_uint, C.c_void_p, C.c_void_p, C.c_void_p, C.c_void_p]
    lib.tg_test_client.argtypes = [C.c_char_p, C.c_char_p, C.c_uint]
    lib.tg_test_client.restype = C.c_void_p
    lib.tg_test_debug.argtypes = [C.c_void_p]
    lib.tg_test_debug.restype = C.c_char_p
    lib.tg_test_receiver.argtypes = [C.c_void_p,C.c_void_p,C.c_void_p]
    lib.tg_test_receiver.restype = C.c_void_p
    lib.tg_receive.argtypes = [C.c_void_p,C.c_void_p,C.c_uint]
    for name in ('step', 'free', 'state', 'revision', 'password_length', 'status', 'qr', 'hint', 'restart'):
        function = getattr(lib, 'tg_client_' + name)
        function.argtypes = [C.c_void_p]
        if name in ('status', 'qr', 'hint'):
            function.restype = C.c_char_p
    lib.tg_client_password_key.argtypes = [C.c_void_p, C.c_int]
    lib.tg_client_password_scalar.argtypes = [C.c_void_p, C.c_uint32]
    lib.tg_client_send.argtypes = [C.c_void_p, C.c_void_p, C.c_uint]
    prime = bytes((C.c_ubyte * 256).in_dll(lib, 'tg_prime'))
    return lib, (n, e, d, fingerprint), prime


def ige(lib, data, key, iv, decrypt=False):
    out = C.create_string_buffer(len(data))
    assert lib.rfl_crypto_call(8 if decrypt else 7, 0, data, len(data), key, 32, iv, 32, out, len(data)) == len(data)
    return out.raw


def password_x(password, salt1, salt2):
    def sh(data, salt):
        return hashlib.sha256(salt + data + salt).digest()
    return int.from_bytes(sh(hashlib.pbkdf2_hmac('sha512', sh(sh(password, salt1), salt2), salt1, 100000), salt2), 'big')
