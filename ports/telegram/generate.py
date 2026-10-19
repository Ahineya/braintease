#!/usr/bin/env python3
"""Generate public trust anchors and selected constructor IDs from pinned TDLib."""
import base64
import hashlib
from pathlib import Path
import re

HERE = Path(__file__).resolve().parent


def item(data):
    tag, length, at = data[0], data[1], 2
    if length & 128:
        n = length & 127
        length, at = int.from_bytes(data[2:2+n], 'big'), 2+n
    return tag, data[at:at+length], data[at+length:]


def tl(data):
    head = bytes([len(data)]) if len(data) < 254 else b'\xfe' + len(data).to_bytes(3, 'little')
    result = head + data
    return result + b'\0' * (-len(result) % 4)


prime = bytes.fromhex(''.join('''
C71CAEB9C6B1C9048E6C522F70F13F73980D40238E3E21C14934D037563D930F
48198A0AA7C14058229493D22530F4DBFA336F6E0AC925139543AED44CCE7C3720
FD51F69458705AC68CD4FE6B6B13ABDC9746512969328454F18FAF8C595F642477
FE96BB2A941D5BCD1D4AC8CC49880708FA9B378E3C4F3A9060BEE67CF9A4A4A6
95811051907E162753B56B0F6B410DBA74D8A84B2A14B3144E0EF1284754FD17
ED950D5965B4B9DD46582DB1178D169C6BC465B0D6FF9CA3928FEF5B9AE4E418
FC15E83EBEA0F87FA9FF5EED70050DED2849F47BF959D956850CE929851F0D811
5F635B105EE2E4E15D04B2454BF6F4FADF034B10403119CD8E3B92FCC5B'''.split()))
assert len(prime) == 256
keys = []
source = (HERE / 'upstream/PublicRsaKeySharedMain.cpp').read_text()
for block in re.findall(r'"-----BEGIN RSA PUBLIC KEY-----.*?-----END RSA PUBLIC KEY-----"', source, re.S):
    pem = ''.join(re.findall(r'"(.*?)"', block, re.S)).replace('\\n', '\n')
    der = base64.b64decode(''.join(pem.splitlines()[1:-1]))
    _, content, tail = item(der)
    _, n, content = item(content)
    _, e, content = item(content)
    assert not content and not tail and e == b'\x01\0\x01'
    n = n.lstrip(b'\0'); assert len(n) == 256
    keys.append((n, hashlib.sha1(tl(n) + tl(e)).digest()[12:]))
assert len(keys) == 2  # test DC key, then production key


def array(data):
    return '{' + ','.join(str(n) for n in data) + '}'


(HERE / 'keys.c').write_text('/* Generated public data; see generate.py and upstream/README.md. */\n#include "core.h"\n' +
    'const TgByte tg_prime[256]=' + array(prime) + ';\n' +
    '#ifndef TG_TEST_KEYS\nconst TgByte tg_rsa_n[2][256]={' + ',\n'.join(array(n) for n, _ in keys) + '};\n' +
    'const TgByte tg_rsa_fingerprint[2][8]={' + ','.join(array(f) for _, f in keys) + '};\n#endif\n')
names = {'auth.exportLoginToken', 'auth.importLoginToken', 'auth.checkPassword', 'account.getPassword',
         'initConnection', 'invokeWithLayer', 'messages.sendMessage', 'inputPeerSelf', 'users.getUsers',
         'inputUserSelf', 'auth.loginToken', 'auth.loginTokenMigrateTo', 'auth.loginTokenSuccess',
         'auth.authorization', 'user', 'account.password', 'inputCheckPasswordSRP', 'updateLoginToken',
         'passwordKdfAlgoSHA256SHA256PBKDF2HMACSHA512iter100000SHA256ModPow',
         'updateShortSentMessage', 'updates', 'updatesCombined', 'updateShort'}
ids = dict(re.findall(r'^([\w.]+)#([0-9a-f]+)\b', (HERE / 'upstream/telegram_api.tl').read_text(), re.M))
assert names <= ids.keys()
names = ids.keys()  # Shared with the schema-driven dialog/message reader.
(HERE / 'schema.h').write_text('/* Generated from pinned API layer 230. */\n#ifndef TG_SCHEMA_H\n#define TG_SCHEMA_H\n#define TG_LAYER 230\n' +
    ''.join('#define TL_' + name.replace('.', '_') + ' 0x' + ids[name] + 'UL\n' for name in sorted(names)) + '#endif\n')
