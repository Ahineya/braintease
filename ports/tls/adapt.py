#!/usr/bin/env python3
"""Build verified C16 working copies; keep the pinned upstream tree unchanged."""
from pathlib import Path
import hashlib
import json
import os
import re
import tempfile

PORT = Path(__file__).resolve().parent
# Match source tokens while retaining byte offsets for minimal, reviewable edits.
TOKEN = re.compile(r'\\\r?\n|/\*.*?\*/|//[^\n]*|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|[A-Za-z_]\w*|(?:0[xX][0-9a-fA-F]+|\d+)[uUlL]*|\+\+|--|->|\S', re.S)
CAST_TYPES = {'byte', 'word16', 'word32', 'word64', 'int', 'unsigned', 'signed',
              'short', 'long', 'char', 'size_t', 'sp_digit', 'sp_int_digit', 'enum',
              'const', 'volatile'}


def write_changed(path, text):
    """Native and RVM builds may share generated sources on a cold build."""
    if path.exists() and path.read_text() == text:
        return
    with tempfile.NamedTemporaryFile(mode='w', dir=path.parent, delete=False) as output:
        output.write(text)
        temporary = output.name
    os.replace(temporary, path)


def octet_casts(source):
    """Restore explicit byte-cast truncation, without changing the C16 ABI.

    Only the cast's unary operand is wrapped: `(byte)x + y` stays octet(x)+y.
    Pointer casts, comments, strings and integer promotions are untouched.
    """
    tokens = [m for m in TOKEN.finditer(source)
              if not m[0].startswith(('/*', '//', '\\\n', '\\\r\n'))]
    values = [m[0] for m in tokens]
    pairs = {}
    stack = []
    for i, token in enumerate(values):
        if token in ('(', '['):
            stack.append(i)
        elif token in (')', ']'):
            if stack:
                start = stack.pop()
                if (values[start], token) not in (('(', ')'), ('[', ']')):
                    raise ValueError('unbalanced source tokens')
                pairs[start] = i
    # Conditional branches may each open/close different braces; braces are
    # irrelevant to unary operand boundaries, so are deliberately not paired.
    def unary(i):
        if values[i] in ('++', '--', '+', '-', '!', '~', '*', '&'):
            return unary(i + 1)
        if values[i] == '(':
            end = pairs[i]
            inside = values[i + 1:end]
            if inside and all(x in CAST_TYPES or x == '*' for x in inside):
                return unary(end + 1)
            i = end + 1
        else:
            i += 1
        while i < len(values):
            if values[i] in ('(', '['):
                i = pairs[i] + 1
            elif values[i] in ('.', '->'):
                i += 2
            elif values[i] in ('++', '--'):
                i += 1
            else:
                break
        return i
    edits = []
    for i in range(len(values) - 3):
        if values[i:i + 3] != ['(', 'byte', ')']:
            continue
        end = unary(i + 3)
        edits.append((tokens[i].start(), tokens[i + 2].end(), 'WC_OCTET('))
        edits.append((tokens[end - 1].end(), tokens[end - 1].end(), ')'))
    for start, end, replacement in sorted(edits, reverse=True):
        source = source[:start] + replacement + source[end:]
    return source, len(edits) // 2


def prepare(destination):
    upstream = PORT / 'upstream'
    manifest = json.loads((PORT / 'upstream-manifest.json').read_text())
    for name, expected in manifest.items():
        if hashlib.sha256((upstream / name).read_bytes()).hexdigest() != expected:
            raise RuntimeError(f'Pinned wolfSSL source changed: {name}')
    destination = Path(destination)
    counts = {}
    for name in manifest:
        source = upstream / name
        output = destination / name
        output.parent.mkdir(parents=True, exist_ok=True)
        data = source.read_text()
        if name.endswith('.c') or name == 'wolfssl/internal.h':
            data, count = octet_casts(data)
            if count:
                counts[name] = count
        if name == 'wolfcrypt/src/sp_int.c':
            # SP_WORD_SIZEOF counts protocol octets. Memory operations count
            # addressable C cells; do not change wire import/export loops.
            old = '(word32)SP_WORD_SIZEOF'
            assert data.count(old) == 8
            data = data.replace(old, '(word32)sizeof(sp_int_digit)')
            old = '(size_t)s * SP_WORD_SIZEOF'
            assert data.count(old) == 1
            data = data.replace(old, '(size_t)s * sizeof(sp_int_digit)')
            old = 'sizeof(i) * 8 - 1'
            assert data.count(old) == 1
            data = data.replace(old, 'sizeof(i) * CHAR_BIT - 1')
        if name == 'wolfcrypt/src/asn.c':
            # Trust anchors contain public certificate data, just like CERT_TYPE
            # below. Avoid the secret-key constant-time PEM decoder for CA files;
            # keep private/encrypted key decoding on the original path.
            old = '    case CERT_TYPE:\n    case TRUSTED_CERT_TYPE:\n    case CRL_TYPE:\n        if (Base64_Decode_nonCT'
            assert data.count(old) == 1
            data = data.replace(old, '    case CA_TYPE:\n' + old)
        if name == 'wolfcrypt/src/sp_c32.c':
            # Keep compact digit arithmetic and variable-point multiplication,
            # but use upstream's precomputed base tables. Unrolling all
            # RSA arithmetic exceeds an RVM function's 64K spill frame; the
            # compact scalar ladder can instead exceed live server deadlines.
            # Scope the independent selector to two complete conditional regions
            # per curve. Preserve the original compact ladder: the windowed path
            # has not passed full-width C16 ECDH vectors. Its helpers are needed
            # by the independently verified base-point stripe implementation.
            for curve,digits in ((256,9),(384,15),(521,21)):
                for base in ('','base_'):
                    function=f'sp_{curve}_ecc_mulmod_{base}{digits}'
                    declaration=data.index('static int '+function+'(')
                    start=data.rfind('#ifdef WOLFSSL_SP_SMALL',0,declaration)
                    assert start>=0 and declaration-start<1200
                    public=f'int sp_ecc_mulmod_{base}{curve}('
                    end=data.index(public,declaration)
                    region=data[start:end]
                    assert '#else' in region and '#endif' in region
                    ladder=''
                    if not base:
                        first=region.index('static int '+function+'(')
                        finish=region.index('\n#ifdef WOLFSSL_SP_NONBLOCK',first)
                        ladder=region[first:finish].rstrip()+'\n\n'
                        assert ladder.rstrip().endswith('}')
                        second=region.index('static int '+function+'(',first+1)
                        region=region[:second]+region[second:].replace(
                            'static int '+function+'(', 'static int '+function+'_window(',1)
                    region=re.sub(r'\bWOLFSSL_SP_SMALL\b','RIPPLE_SP_SMALL_SCALAR',region)
                    data=data[:start]+ladder+region+data[end:]
        if name == 'src/internal.c':
            old = 'sizeof(ret) * 8'
            assert data.count(old) == 1
            data = data.replace(old, 'sizeof(ret) * CHAR_BIT')
        if name == 'wolfssl/wolfcrypt/wc_port.h':
            old = '#define WC_INIT_STATE_COUNT_BITS ((sizeof(WC_ATOMIC_UINT_ARG) * 8) - WC_INIT_STATE_STATE_BITS)'
            assert data.count(old) == 1
            data = data.replace(old, old.replace('* 8', '* CHAR_BIT'))
        write_changed(output, data)
    report = dict(explicit_octet_casts=counts, init_counter_bits='sizeof(unsigned int) * CHAR_BIT - 3', sp_memory_cell_counts=9, integer_sign_widths=2, fast_ecc_base_tables=3, compact_ecc_variable_multiply=3, public_ca_base64_decoder=1)
    write_changed(destination / 'ripple-adaptations.json', json.dumps(report, indent=2) + '\n')
    return destination


if __name__ == '__main__':
    import argparse
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    print(prepare(args.output))
