#!/usr/bin/env python3
"""Validate string/typed-array storage changes on complete host engines."""
import json
import os
from pathlib import Path
import subprocess
import sys
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from adapt import replace
from core_storage import string_storage
from core_bytecode import bytecode_operands
from core_heap import arena_storage, atom_storage, closure_storage, core_integer_constants
from core_libc import parser_formats, libc_formats
from core_typed_arrays import typed_array_storage
from vendor import PORT, verify

ROOT = PORT.parents[1]
verify()
build = ROOT / 'build/quickjs-storage/full-host'
build.mkdir(parents=True, exist_ok=True)
core_source = (PORT / 'upstream/quickjs.c').read_text()
for transform in (string_storage, bytecode_operands, arena_storage, atom_storage, closure_storage,
                  core_integer_constants, parser_formats, typed_array_storage, libc_formats):
    core_source = transform(core_source)
(build / 'quickjs.c').write_text(core_source)
embed = (PORT / 'tests/embed.c').read_text()
embed = replace(embed, 'static unsigned checks, callbacks, interrupts, unhandled;', '''static unsigned checks, callbacks, interrupts, unhandled;
static JSValue native_bank_offset(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv)
{
    (void)self; (void)argc; (void)argv;
    return JS_NewInt32(ctx, 63999); /* flat host reference; real edges in C16 */
}
static JSValue roundtrip(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv)
{
    size_t size;
    uint8_t *data;
    JSValue value;
    (void)self;
    if (argc != 1) return JS_ThrowTypeError(ctx, "roundtrip expects one argument");
    data = JS_WriteObject(ctx, &size, argv[0], JS_WRITE_OBJ_BYTECODE);
    if (!data) return JS_EXCEPTION;
    value = JS_ReadObject(ctx, data, size, JS_READ_OBJ_BYTECODE);
    js_free(ctx, data);
    return value;
}
''')
embed = replace(embed, '    if (!eval_ok(ctx, source, JS_EVAL_TYPE_GLOBAL)) goto done;', '''    if (add_function(ctx, global, "nativeRoundTrip", roundtrip, 1) < 0) goto done;
    if (add_function(ctx, global, "nativeBankOffset", native_bank_offset, 1) < 0) goto done;
    {
        JSValue compiled, restored, result;
        compiled = JS_Eval(ctx, "(() => 6 * 7)()", 15, "compiled.js",
                           JS_EVAL_TYPE_GLOBAL | JS_EVAL_FLAG_COMPILE_ONLY);
        if (JS_IsException(compiled)) goto done;
        restored = roundtrip(ctx, JS_UNDEFINED, 1, &compiled);
        JS_FreeValue(ctx, compiled);
        if (JS_IsException(restored)) goto done;
        result = JS_EvalFunction(ctx, restored);
        if (!JS_IsNumber(result) || JS_VALUE_GET_INT(result) != 42) {
            JS_FreeValue(ctx, result); goto done;
        }
        JS_FreeValue(ctx, result); ++checks;
    }
    if (!eval_ok(ctx, source, JS_EVAL_TYPE_GLOBAL)) goto done;''')
# Use strlen rather than an independently maintained source length.
embed = replace(embed, '"(() => 6 * 7)()", 15,', '"(() => 6 * 7)()", strlen("(() => 6 * 7)()"),')
(build / 'embed.c').write_text(embed)
# Exercise the portable directive-precision repair in a complete host engine,
# as well as the C16 guest evaluation fixture.
semantics = "'use strict';\n" + (PORT / 'tests/semantics.js').read_text() + '\n' + (PORT / 'tests/typed-arrays.js').read_text() + '\n' + (PORT / 'tests/formats.js').read_text() + r'''
const text = 'A\u00e9\u0416\u4e2d\ud83d\ude00\0Z';
check(nativeRoundTrip(text) === text && text.length === 8, 'serialized UTF16 with NUL');
check(nativeRoundTrip('\u00ff\0') === '\u00ff\0', 'serialized Latin1 with NUL');
check(nativeRoundTrip({a:text,b:[-2147483648,4294967295,1.25,2n**100n]}).b[3] === 2n**100n,
      'object numeric and wide string serialization');
check('\0' < '\uffff' && '\uffff' > '\0' && '\0x'.slice(0,1) < '\u00ff', 'code unit ordering');
check(text.slice(4,6) === '\ud83d\ude00' && text.slice(0,2) === 'A\u00e9', 'wide substring and narrowing');
check(Array.from(text).length === 7 && text.codePointAt(4) === 0x1f600, 'code point iteration');
check(text.replace(/(\u0416)(\u4e2d)/u,'$2$1') === 'A\u00e9\u4e2d\u0416\ud83d\ude00\0Z',
      'wide capture substitution');
const match = /(?<face>\u{1f600})/du.exec(text);
check(match.index === 4 && match.indices.groups.face[0] === 4 && match.indices.groups.face[1] === 6,
      'wide regexp capture indices');
check(text.replace(/\u{1f600}/gu,'<$&>') === 'A\u00e9\u0416\u4e2d<\ud83d\ude00>\0Z',
      'wide regexp global replacement');
check(text.split(/\u4e2d/u)[1] === '\ud83d\ude00\0Z', 'wide regexp split');
let rope=''; for(let i=0;i<2000;i++) rope += 'x\u0416';
check(rope.length === 4000 && nativeRoundTrip(rope).slice(-4) === 'x\u0416x\u0416', 'rope flatten and serialize');
check('\ud800x\udc00'.toWellFormed() === '\ufffdx\ufffd' && !'\ud800'.isWellFormed(), 'unpaired surrogate handling');
'''
(build / 'semantics.js').write_text(semantics)
flags = ['-std=gnu11', '-O2', '-fwrapv', '-D_GNU_SOURCE', '-DCONFIG_VERSION="2026-06-04"',
         '-I' + str(PORT / 'upstream')]
if sys.platform == 'darwin':
    sdk = os.environ.get('SDKROOT') or subprocess.check_output(
        ['xcrun', '--sdk', 'macosx', '--show-sdk-path'], text=True).strip()
    flags += ['-isysroot', sdk]
report = {}
for profile, core in [('reference', PORT / 'upstream/quickjs.c'), ('native-string-units', build / 'quickjs.c')]:
    binary = build / profile
    with (build / (profile + '-build.log')).open('w') as log:
        subprocess.run(list(map(str, [os.environ.get('CC', 'cc'), *flags, build / 'embed.c', core,
            *[PORT / 'upstream' / (name + '.c') for name in ('dtoa', 'libregexp', 'libunicode', 'cutils')],
            '-lm', '-o', binary])), check=True, stdout=log, stderr=subprocess.STDOUT, timeout=180)
    result = subprocess.run([str(binary), str(build / 'semantics.js')], capture_output=True, text=True, timeout=30)
    output = result.stdout + result.stderr
    (build / (profile + '.log')).write_text(output)
    if result.returncode or 'QUICKJS_REFERENCE checks=92 ' not in output:
        raise RuntimeError(profile + ' engine failed: ' + output)
    report[profile] = output.strip()
    print(profile + ': ' + output.strip(), flush=True)
    for label, source in [('assertion', "nativeAssert(false,'negative control')"),
                          ('promise', "globalThis.asyncDone=true; Promise.resolve().then(()=>nativeAssert(false,'async failure'))")]:
        path = build / (label + '.js'); path.write_text(source)
        negative = subprocess.run([str(binary), str(path)], capture_output=True, timeout=30)
        if negative.returncode != 1 or b'QUICKJS_REFERENCE' in negative.stdout:
            raise RuntimeError(profile + ' negative control failed: ' + label)
if report['reference'] != report['native-string-units']:
    raise RuntimeError('Host storage adaptation changed retained memory or semantic counters')
(build / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
