#!/usr/bin/env python3
"""Compile the adapted core to IR; this entry point never links an engine.

This draft retains unaudited engine paths, including auxiliary octet conversions,
debug/serialization paths and broader conformance. Runtime/context, evaluation and
full-intrinsic gates are separate from this compilation probe;
compiling to IR is not a porting success.
The already tested platform hooks are connected here so failures past this
point can be investigated without disabling stack/memory limits.
The separate explore.py --only runtime fixture tests runtime/context integration;
it is not an installed embedding SDK.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess

from adapt import prepare, private_ints, replace, function_body
from vendor import PORT, verify
from core_frames import heap_frames
from core_storage import string_storage, serialized_storage
from core_bytecode import bytecode_operands
from core_heap import arena_storage, atom_storage, closure_storage, core_integer_constants
from core_libc import parser_formats, libc_formats
from core_typed_arrays import typed_array_storage

ROOT = PORT.parents[1]


def prepare_probe(destination):
    destination = prepare(destination)
    source = (PORT / 'upstream/quickjs.c').read_text()
    for transform in (heap_frames, string_storage, serialized_storage, bytecode_operands,
                      arena_storage, atom_storage, closure_storage, core_integer_constants,
                      parser_formats, typed_array_storage, libc_formats, private_ints):
        source = transform(source)
    source = replace(source, '#include "dtoa.h"',
                     '#include "dtoa.h"\n#include "qjs-platform.h"')
    # The current guest libc's localtime is UTC and has no tm_gmtoff field.
    source = function_body(source, 'getTimezoneOffset', '    (void)time;\n    return 0; /* C16 guest UTC profile */')
    source = replace(source, '#define DIRECT_DISPATCH  1', '#define DIRECT_DISPATCH  0')
    source = replace(source, '#define CONFIG_ATOMICS',
                     '/* C16 first profile: single threaded, no Atomics. */')
    # This is deliberately NOT an __EMSCRIPTEN__ profile: retain stack checks.
    start = source.index('#if !defined(CONFIG_STACK_CHECK)')
    end = source.index('JSRuntime *JS_NewRuntime2', start)
    source = source[:start] + '''#ifndef CONFIG_STACK_CHECK
#error C16 core probe requires stack checks
#endif
static inline uintptr_t js_get_stack_pointer(void)
{
    volatile qjs_c_int marker = 0;
    return qjs_c16_linear_address((const void *)&marker);
}
static inline BOOL js_check_stack_overflow(JSRuntime *rt, size_t requested)
{
    return qjs_c16_check_stack(rt->stack_top, rt->stack_size, requested);
}

''' + source[end:]
    # stack_limit was a downward-only boundary. Checks now use base + distance;
    # the field is retained in this draft to minimize unrelated layout changes.
    source = function_body(source, 'update_stack_limit', '    rt->stack_limit = 0;')
    for name, field, parameters in (
        ('js_def_malloc', 'js_malloc', 's, size'),
        ('js_def_free', 'js_free', 's, ptr'),
        ('js_def_realloc', 'js_realloc', 's, ptr, size'),
        ('js_def_malloc_usable_size', 'js_malloc_usable_size', 'ptr'),
    ):
        result = '' if field == 'js_free' else 'return '
        source = function_body(source, name,
                               f'    {result}qjs_c16_malloc_functions.{field}({parameters});')
    # The C16 provider and arena layout explicitly preserve tag alignment;
    # the guest compiler does not implement GNU alignment attributes.
    source = replace(source, '__attribute__((aligned(JS_MALLOC_ALIGN))) ', '', count=3)
    source = replace(source, '__attribute__((warn_unused_result))', '')
    source = replace(source, '__attribute((unused)) ', '')
    source, count = re.subn(r'__attribute__\(\(format\(printf, [234], [345]\)\)\) ?', '', source)
    if count != 14:
        raise ValueError(f'Unexpected core printf attribute count: {count}')
    # Upstream nests a flexible-array-containing struct, a GNU extension
    # forbidden by C99. RCC's existing zero-length-tail extension represents
    # that draft layout without adding a storage cell.
    source = replace(source, 'qjs_octet_t user_data[];', 'qjs_octet_t user_data[0];')
    (destination / 'quickjs.c').write_text(source)
    for name in ('list.h', 'quickjs-atom.h', 'quickjs-opcode.h'):
        (destination / name).write_bytes((PORT / 'upstream' / name).read_bytes())
    # The same language vectors are read by the host oracle and compiled into
    # the guest harness. Keep JavaScript escapes distinct from C escapes.
    fixture = '\n'.join((PORT / ('tests/' + name)).read_text()
                        for name in ('semantics.js', 'typed-arrays.js', 'formats.js'))
    (destination / 'builtin-fixture.inc').write_text(json.dumps(fixture, ensure_ascii=False) + '\n')
    return destination


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', type=Path, default=ROOT / 'build/quickjs-explore')
    parser.add_argument('--compiler', type=Path, default=ROOT / 'target/release/rcc')
    parser.add_argument('--target', choices=('native', 'ripple', 'both'), default='both')
    parser.add_argument('--timeout', type=int, default=300)
    args = parser.parse_args()
    if args.timeout < 1: parser.error('--timeout must be positive')
    verify()
    build, requested = args.build.resolve(), args.compiler.resolve()
    build.mkdir(parents=True, exist_ok=True)
    compiler = build / 'core-rcc-pinned'
    if requested != compiler and (not compiler.exists() or
            hashlib.sha256(requested.read_bytes()).digest() != hashlib.sha256(compiler.read_bytes()).digest()):
        shutil.copy2(requested, compiler)
    source = prepare_probe(build / 'core-probe')
    report = dict(scope='Compile-only draft, NOT a runnable or audited engine',
                  guest_javascript_evaluation=False,
                  compiler_sha256=hashlib.sha256(compiler.read_bytes()).hexdigest(), targets={})
    includes = [source, ROOT / 'ports/fuzix/ripple/include',
                ROOT / 'ports/fuzix/upstream/Library/include', ROOT / 'runtime/include']
    success = True
    for target in (('native', 'ripple') if args.target == 'both' else (args.target,)):
        log, ir = source / (target + '.log'), source / (target + '.ir')
        command = [str(compiler), 'compile', str(source / 'quickjs.c'), '--target', target,
                   '-DRIPPLE_MMU=1', '-D_BSD_SOURCE=1', '-DCONFIG_VERSION="2026-06-04"', '--emit-ir', '--timings',
                   *[v for directory in includes for v in ('-I', str(directory))]]
        with ir.open('w') as output, log.open('w') as errors:
            try:
                process = subprocess.run(command, stdout=output, stderr=errors,
                                         timeout=args.timeout, env=dict(os.environ, RUST_LOG='error'))
                status = 'compiled-to-ir' if not process.returncode else 'blocked'
                code = process.returncode
            except subprocess.TimeoutExpired:
                errors.write('\nCore compiler probe timed out\n')
                status, code = 'timeout', None
        diagnostic = log.read_text()[-4000:]
        report['targets'][target] = dict(status=status, exit_code=code,
                                          diagnostic=diagnostic, log=str(log))
        if status != 'compiled-to-ir':
            success = False
            ir.unlink()  # Never leave partial output looking like usable IR.
        print(target + ': ' + status + '\n' + diagnostic, flush=True)
        (build / 'core-report.json').write_text(json.dumps(report, indent=2) + '\n')
    return 0 if success else 1


if __name__ == '__main__':
    raise SystemExit(main())
