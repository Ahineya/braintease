#!/usr/bin/env python3
"""Host resource regressions for the portable lowerer and linker.

The budget counts tool allocations in host bytes, not guest C16 cells. Actual
large-source guest acceptance is separate. Outputs must match an unlimited run.
"""
import argparse
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

HERE = Path(__file__).resolve().parent
PORTABLE = HERE.parent


def build(work, name, sources, budget):
    cc = [*shlex.split(os.environ.get('CC', 'cc')), '-std=c99', '-O2', '-Wall', '-Wextra']
    if os.uname().sysname == 'Darwin':
        cc += ['-isysroot', os.environ.get('SDKROOT') or subprocess.check_output(
            ['xcrun', '--show-sdk-path'], text=True).strip()]
    if budget:
        helper = work / 'budget.o'
        subprocess.run([*cc, '-c', str(HERE / 'tests/heap_budget.c'), '-o', str(helper)], check=True)
        cc += ['-D' + s + '=budget_' + s for s in ['malloc', 'calloc', 'realloc', 'free']]
        sources = [*sources, helper]
    output = work / (name + ('-budget' if budget else ''))
    subprocess.run([*cc, *map(str, sources), '-o', str(output)], check=True)
    return output


def check(tool, arguments, output, limit, max_request=None):
    output.write_bytes(b'preserve')
    env = dict(os.environ, RCC_TEST_HEAP_LIMIT=str(limit))
    if max_request:
        env['RCC_TEST_MAX_REQUEST'] = str(max_request)
    else:
        env.pop('RCC_TEST_MAX_REQUEST', None)
    result = subprocess.run([str(tool), *map(str, arguments), '-o', str(output)],
                            env=env, capture_output=True, text=True)
    assert result.returncode == 0, (tool.name, result.stderr)
    assert 'HEAP peak=' in result.stderr, 'allocation budget was not active'
    print(tool.name + ': ' + result.stderr.strip(), flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--assembler', type=Path, default=PORTABLE / 'build/rcc-x64-as')
    parser.add_argument('--budget-tools', type=Path, help='optional prebuilt allocation-instrumented tools')
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix='rcc-memory-') as folder:
        work = Path(folder)
        lower_sources = [*sorted((PORTABLE / 'native').glob('*.c')),
                         PORTABLE / 'ripple/parse.c', PORTABLE / 'ripple/helpers.c']
        link_sources = sorted((PORTABLE / 'x64-link').glob('*.c'))
        tools = {}
        for name, sources in [('rcc-qbe', lower_sources), ('rcc-x64-ld', link_sources)]:
            tools[name] = (args.budget_tools.resolve() / name if args.budget_tools
                           else build(work, name, sources, True))
            tools[name + '-reference'] = build(work, name, sources, False)
        # Thousands of ordinary instructions need no per-instruction liveness
        # snapshot. Calls, loop checkpoints and phi edge reloads have execution
        # coverage in native/test.py; this catches the quadratic space cost.
        ir = work / 'large.ir'
        body = ['module memory { target native function i16 @main() { block 0:',
                '%0 = add i16 0, 1']
        body += [f'%{i} = add i16 %{i-1}, 1' for i in range(1, 2500)]
        body += ['ret %2499 } }']
        ir.write_text('\n'.join(body) + '\n')
        lowered = work / 'large.ssa'
        check(tools['rcc-qbe'], [ir], lowered, 4 * 1024 * 1024)
        reference = work / 'reference.ssa'
        subprocess.run([str(tools['rcc-qbe-reference']), str(ir), '-o', str(reference)], check=True)
        assert lowered.read_bytes() == reference.read_bytes()
        # Inputs plus output formerly exceeded 12 MiB. The sections themselves
        # are real ELF payloads, not BSS: they must survive the streamed copy.
        objects = []
        for i in range(3):
            source = work / f'part{i}.s'
            obj = work / f'part{i}.o'
            source.write_text('.text\n' + ('.globl entry\nentry: ret\n' if not i else '')
                              + f'.byte {i+17}\n.zero 2700000\n.byte {i+27}\n')
            subprocess.run([str(args.assembler.resolve()), str(source), '-o', str(obj)], check=True)
            objects.append(obj)
        meta = work / 'entry.ssa'
        meta.write_text('# rcc-entry 1\n# rcc-function 1 1 entry\n')
        linked = work / 'large.fxe'
        arguments = ['--metadata', meta, *objects]
        # Model a fragmented heap too: plenty of free total storage need not
        # provide one contiguous 8 MiB block for the final image.
        check(tools['rcc-x64-ld'], arguments, linked, 12 * 1024 * 1024, 5 * 1024 * 1024)
        # An independent payload check also covers unaligned copies and output
        # writes crossing many chunk boundaries, including the last short one.
        for i in range(3):
            pattern = bytes([i + 17]) + bytes(2700000) + bytes([i + 27])
            assert linked.read_bytes().count(pattern) == 1
        reference = work / 'reference.fxe'
        subprocess.run([str(tools['rcc-x64-ld-reference']), *map(str, arguments),
                        '-o', str(reference)], check=True)
        assert linked.read_bytes() == reference.read_bytes()
        # Relocation tables must not all remain resident either. Each object
        # has many real PC-relative call relocations; only one table is needed
        # while patching its own already-laid-out section.
        relocations = []
        for i in range(3):
            source = work / f'reloc{i}.s'
            obj = source.with_suffix('.o')
            source.write_text('.text\n' + ('.globl entry\nentry:\n' if not i else '')
                              + 'call target\n' * 50000 + 'ret\n'
                              + ('.globl target\ntarget: ret\n' if not i else ''))
            subprocess.run([str(args.assembler.resolve()), str(source), '-o', str(obj)], check=True)
            relocations.append(obj)
        arguments = ['--metadata', meta, *relocations]
        check(tools['rcc-x64-ld'], arguments, linked, 7 * 1024 * 1024)
        subprocess.run([str(tools['rcc-x64-ld-reference']), *map(str, arguments),
                        '-o', str(reference)], check=True)
        assert linked.read_bytes() == reference.read_bytes()
        cache = build(work, 'type-cache', [HERE / 'tests/type_cache.c',
                      PORTABLE / 'ripple/parse.c', PORTABLE / 'ripple/helpers.c'], False)
        subprocess.run([str(cache)], check=True)
        changed = build(work, 'changed-input', [HERE / 'tests/changed_input.c',
                        *[s for s in link_sources if s.name != 'main.c']], False)
        # Refuse a changed input before publishing an otherwise valid image.
        damaged = work / 'changed.o'
        damaged.write_bytes(objects[0].read_bytes())
        linked.write_bytes(b'preserve')
        result = subprocess.run([str(changed), str(meta), str(damaged), str(linked)],
                                capture_output=True, text=True)
        assert result.returncode == 1 and 'input changed' in result.stderr, result
        assert linked.read_bytes() == b'preserve'
    print('Bounded lowerer/linker allocations and unlimited-output parity passed')


if __name__ == '__main__':
    main()
