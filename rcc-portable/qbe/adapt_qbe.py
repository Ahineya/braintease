#!/usr/bin/env python3
"""Generate the pinned QBE sources for the C16 bootstrap compiler.

QBE's target sizes are octets; sizeof/malloc below remain C storage units.
Keep upstream unchanged. The bootstrap uses host RCC; a portable-frontend
bootstrap is a later gate (notably bitfields and flexible array members).
"""
import ast
from pathlib import Path
import re
from selector import split_selector
from coalesce import split_coalesce
from sysv import split_sysv_call
from registers import split_registers

HERE = Path(__file__).resolve().parent
PORTABLE = HERE.parent
UPSTREAM = PORTABLE / 'upstream/qbe'
OUTPUT = PORTABLE / 'build/qbe-c16'
COMMON = 'main util parse abi cfg mem ssa alias load copy fold gvn gcm simpl ifopt live spill rega emit'.split()
SOURCES = [name + '.c' for name in COMMON]
SOURCES += ['amd64/' + name + '.c' for name in 'targ sysv isel emit'.split()]
TOKEN = re.compile(r'//[^\n]*|/\*[\s\S]*?\*/|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|'
                   r'\b0[xX](?:[0-9a-fA-F]+\.?[0-9a-fA-F]*|\.[0-9a-fA-F]+)[pP][+-]?\d+[fFlL]?|'
                   r'(?:\b\d+\.\d*|\.\d+|\b\d+[eE][+-]?\d+)(?:[eE][+-]?\d+)?[fFlL]?|'
                   r'\b(?:0[xX][0-9a-fA-F]+|[0-9]+)(?:[uUlL]+)?\b|\b[A-Za-z_][A-Za-z_0-9]*\b|.', re.S)
FORMAT = re.compile(r'%%|%[-+ #0]*(?:\d+|\*)?(?:\.(?:\d+|\*))?(?:ll|l|z|j|hh|h)?[diuoxXcspfFeEgG]')
PRI = {'PRId32': 'ld', 'PRId64': 'lld', 'PRIi64': 'lli', 'PRIu64': 'llu'}


def replace_once(source, old, new):
    if source.count(old) != 1:
        raise RuntimeError('QBE adaptation contract changed: ' + old)
    return source.replace(old, new)


def parallel_moves(source):
    """Size edge-copy storage with the function, retaining QBE's move order."""
    source = replace_once(source, 'static struct {\n\tRef src, dst;\n\tint cls;\n} pm[NPm];',
                          'struct ParallelMove {\n\tRef src, dst;\n\tint cls;\n};\n'
                          'static struct ParallelMove *pm;\nstatic int pm_capacity;')
    source = replace_once(source, 'if (npm == NPm)', 'if (npm == pm_capacity)')
    source = replace_once(source, '\t/* 1. setup */', '''\t/* Include spilled phi/edge copies, not only hardware registers. */
    if (fn->ntmp > (INT_MAX - Tmp0) / 2)
        die("parallel move capacity overflow");
    pm_capacity = 2 * fn->ntmp + Tmp0;
    pm = alloc((size_t)pm_capacity * sizeof *pm);

\t/* 1. setup */''')
    return source


def format_string(token):
    def widen(match):
        value = match[0]
        if value == '%%':
            return value
        if value[-1] in 'diuoxX' and not re.search(r'[hlzj]', value[1:]):
            return value[:-1] + 'l' + value[-1]
        return value
    return FORMAT.sub(widen, token)


def widen_integers(source):
    """32-bit compiler indices must not become the guest's 16-bit C int.

    Do not rewrite identifiers, comments, character literals or assembly text.
    Varargs formats are adapted along with the corresponding call operands.
    """
    result = []
    tokens = TOKEN.findall(source)
    for i, token in enumerate(tokens):
        if token.startswith('"'):
            token = format_string(token)
        elif token == 'int':
            token = 'long'
        elif token == 'unsigned':
            following = next((t for t in tokens[i + 1:] if not t.isspace()), '')
            if following not in ('char', 'short', 'int', 'long'):
                token = 'unsigned long'
        elif token in ('INT_MAX', 'UINT_MAX', 'CHAR_MAX', 'UCHAR_MAX'):
            token = dict(INT_MAX='2147483647L', UINT_MAX='4294967295UL',
                         CHAR_MAX='127L', UCHAR_MAX='255UL')[token]
        elif re.fullmatch(r'(?:0[xX][0-9a-fA-F]+|[0-9]+)', token):
            token += 'L'
        elif re.fullmatch(r'(?:0[xX][0-9a-fA-F]+|[0-9]+)[uU]', token):
            token = token[:-1] + 'UL'
        result.append(token)
    return ''.join(result)


def format_calls(source):
    """Match default promotions to the widened format, including char operands.

    Only literal formats are rewritten. Dynamic forwarding uses the unchanged
    va_list interface. Width/character parameters still use native C16 int.
    """
    names = {'printf': 0, 'fprintf': 1, 'sprintf': 1, 'snprintf': 2,
             'die': 0, 'err': 0, 'strf': 1}
    tokens = TOKEN.findall(source)
    changes = []
    for i, token in enumerate(tokens):
        if token not in names:
            continue
        j = i + 1
        while j < len(tokens) and tokens[j].isspace():
            j += 1
        if j == len(tokens) or tokens[j] != '(':
            continue
        begin = j + 1
        depth = 0
        args = []
        for k in range(begin, len(tokens)):
            t = tokens[k]
            if t in ('(', '[', '{'):
                depth += 1
            elif t in (')', ']', '}'):
                if depth == 0:
                    args.append((begin, k))
                    break
                depth -= 1
            if t == ',' and depth == 0:
                args.append((begin, k))
                begin = k + 1
        at = names[token]
        if len(args) <= at:
            continue
        pieces = [t for t in tokens[slice(*args[at])] if not t.isspace()]
        if not pieces or any(not t.startswith('"') and t not in PRI for t in pieces):
            continue
        fmt = ''.join(ast.literal_eval(t) if t.startswith('"') else PRI[t] for t in pieces)
        casts = []
        for match in FORMAT.finditer(fmt):
            spec = match[0]
            if spec == '%%':
                continue
            casts += ['short'] * spec.count('*')
            conversion = spec[-1]
            if conversion in 'diuoxX':
                unsigned = conversion in 'uoxX'
                width = 'long long' if 'll' in spec or 'j' in spec else 'long'
                casts.append(('unsigned ' if unsigned else '') + width)
            elif conversion == 'c':
                casts.append('short')
            else:
                casts.append(None)
        for span, cast in zip(args[at + 1:], casts):
            if cast:
                lo, hi = span
                changes += [(lo, '(' + cast + ')('), (hi, ')')]
    inserts = {}
    for index, text in changes:
        inserts[index] = inserts.get(index, '') + text
    return ''.join(inserts.get(i, '') + t for i, t in enumerate(tokens)) + inserts.get(len(tokens), '')


def adapt(name, source):
    # GCC's declaration attribute is not part of the bootstrap C99 grammar.
    # Both functions unconditionally terminate; this removes only an annotation.
    source = source.replace('__attribute__((noreturn))', '')
    # Recorded RED: test_function_parameter_declarator_decay. Explicit pointer
    # spelling has exactly the same C type as a function parameter declarator.
    for old, new in [('int f(Fn *, Ref, int)', 'int (*f)(Fn *, Ref, int)'),
                     ('void f(Blk *, Blk *)', 'void (*f)(Blk *, Blk *)'),
                     ('void cb(Dat *)', 'void (*cb)(Dat *)'),
                     ('void dbgfile(char *)', 'void (*dbgfile)(char *)'),
                     ('void data(Dat *)', 'void (*data)(Dat *)'),
                     ('void func(Fn *)', 'void (*func)(Fn *)'),
                     ('void (char *)', 'void (*)(char *)'),
                     ('void (Dat *)', 'void (*)(Dat *)'),
                     ('void (Fn *)', 'void (*)(Fn *)')]:
        source = source.replace(old, new)
    if name == 'all.h':
        source = replace_once(source, '\tNIns    = 1 << 20,', '')
        source = '#define NIns qbe_instruction_capacity\nextern unsigned long qbe_instruction_capacity;\n' + source
        # Recorded RED: test_nested_struct_tag_completion. Keep the same type
        # definition and layout, but declare the tag at file scope explicitly.
        begin = source.index('\tstruct Field {')
        end = source.index('\t} (*fields)[NField+1];', begin)
        field = source[begin:end] + '\t};\n'
        source = source[:begin] + '\tstruct Field (*fields)[NField+1];' + source[end + len('\t} (*fields)[NField+1];'):]
        source = source.replace('struct Typ {', field + '\nstruct Typ {')
        # Recorded RED: test_nonvoid_exit_terminator. The bootstrap compiler
        # does not infer termination from exit/abort or noreturn declarations.
        source = replace_once(source, '#define die(...) die_(__FILE__, __VA_ARGS__)',
                              '#define die(...) do { die_(__FILE__, __VA_ARGS__); for (;;) {} } while (0)')
        source = replace_once(source, 'extern Ins insb[NIns], *curi;', 'extern Ins *insb, *curi;')
        source += '\n#include "guest.h"\n'
    elif name == 'util.c':
        source = replace_once(source, '\tVMag = 0xcabba9e,', '')
        source = '#define VMag 0xcabba9eUL\n' + source
        source = replace_once(source, 'Ins insb[NIns], *curi;', 'Ins *insb, *curi;')
        source = replace_once(source, 'long double ld;', 'long long ld;')
        source = replace_once(source, 'die("emit, too many instructions");',
                              'qbe_scratch_exhausted();')
        begin = source.index('\nvoid *\nalloc(size_t n)')
        end = source.index('\nvoid *\nvnew(', begin)
        source = source[:begin] + '\n#include "arena.inc"\n' + source[end:]
        source = replace_once(source, 'static void *ptr[NPtr];\nstatic void **pool = ptr;\nstatic int nptr = 1;', '')
    elif name == 'main.c':
        for target in ('arm64', 'arm64_apple', 'rv64', 'amd64_apple', 'amd64_win'):
            source = source.replace('extern Target T_' + target + ';', '')
            source = source.replace('\t&T_' + target + ',\n', '')
        source = replace_once(source, 'main(int ac, char *av[])', 'qbe_main(int ac, char *av[])')
        source = replace_once(source, '\n\tdo {\n', '\n\tinsb = emalloc(NIns * sizeof *insb);\n\tdo {\n')
        source = replace_once(source, '\n\texit(0);\n}', '''
    if (fflush(outf) || ferror(outf)) {
        fprintf(stderr, "qbe: cannot write output\\n");
        return 1;
    }
    if (outf != stdout && fclose(outf)) {
        fprintf(stderr, "qbe: cannot close output\\n");
        return 1;
    }
    qbe_output_ready();
    return 0;
}''')
        source = source.replace('exit(0);', 'return 0;').replace("exit(c != 'h');", "return c != 'h';")
        source = source.replace('exit(1);', 'return 1;')
    elif name == 'fold.c':
        source = replace_once(source, '(int8_t)l.u', '(int32_t)((l.u & 255) ^ 128) - 128')
        source = replace_once(source, '(uint8_t)l.u', '(l.u & 255)')
    elif name == 'rega.c':
        source = parallel_moves(source)
        source = split_registers(source)
    elif name == 'mem.c':
        source = split_coalesce(source)
    elif name == 'amd64/targ.c':
        # Only SysV is exposed by the guest command. Do not link unreachable
        # Apple/Windows ABI implementations into the C16 executable.
        begin = source.index('\nTarget T_amd64_apple = {')
        source = source[:begin] + '\n'
    elif name == 'amd64/emit.c':
        begin = source.index('\nstatic void\nwinabi_framesz(E *e)')
        source = source[:begin] + '\n'
    elif name == 'amd64/sysv.c':
        source = split_sysv_call(source)
    elif name == 'spill.c':
        # limit2 runs once per instruction; dopm calls it while its own saved
        # set is live. Retaining a fresh full-width set for every call made
        # spilling quadratic in function size. These two distinct workspaces
        # are overwritten before use and initialized for each spill(fn), so
        # freeall() keeps their original function lifetime without stale reuse.
        source = replace_once(source, 'static BSet mask[2][1];',
                              'static BSet qbe_limit_scratch[1], qbe_dopm_scratch[1];\n'
                              'static BSet mask[2][1];')
        source = replace_once(source,
                              '\tBSet b2[1];\n\n\tbsinit(b2, ntmp); /* todo, free those */',
                              '\tBSet *b2 = qbe_limit_scratch;\n')
        source = replace_once(source, '\tBSet u[1];', '\tBSet *u = qbe_dopm_scratch;')
        source = replace_once(source, '\tbsinit(u, ntmp); /* todo, free those */\n', '')
        source = replace_once(source, '\tbsinit(mask[1], ntmp);',
                              '\tbsinit(mask[1], ntmp);\n'
                              '\tbsinit(qbe_limit_scratch, ntmp);\n'
                              '\tbsinit(qbe_dopm_scratch, ntmp);')
        # The bootstrap native backend gives this large resumable function a
        # 121 KiB machine frame, beyond the x64 kernel's guarded 64 KiB stack.
        # Keep the algorithm unchanged, but compile each block as its own step.
        begin = source.index('\t\t/* invariant: all blocks with bigger rpo')
        end = source.index('\n\t}\n\n\t/* align the locals', begin)
        body = source[begin:end]
        ibegin = body.index('\t\t\ti--;')
        iend = body.index('\n\t\t}\n\t\tif (b == fn->start)', ibegin)
        instruction = body[ibegin:iend].replace('continue;', 'return i;')
        body = body[:ibegin] + '\t\t\ti = qbe_spill_instruction(fn, b, i, u, v, w);' + body[iend:]
        instruction_helper = '''static Ins *
qbe_spill_instruction(Fn *fn, Blk *b, Ins *i, BSet *u, BSet *v, BSet *w)
{
    int j, t, lvarg[2];
    uint n;
    Mem *m;
    bits r;
''' + instruction + '\n    return i;\n}\n\n'
        helper = '''static void
qbe_spill_block(Fn *fn, Blk *b, BSet *u, BSet *v, BSet *w)
{
    Blk *s1, *s2, *hd;
    int j, l, t, k, lvarg[2];
    uint n;
    Ins *i;
    Phi *p;
    Mem *m;
    bits r;
''' + body + '\n}\n\n'
        source = source[:begin] + '\t\tqbe_spill_block(fn, b, u, v, w);' + source[end:]
        source = source.replace('void\nspill(Fn *fn)', instruction_helper + helper + 'void\nspill(Fn *fn)')
    elif name == 'parse.c':
        source = replace_once(source, '\tK = 11183273, /* found using tools/lexh.c */', '')
        source = '#define K 11183273UL /* found using tools/lexh.c */\n' + source
        source = replace_once(source, 'err(char *s, ...)', 'qbe_error(char *s, ...)')
        source = source.replace('fscanf(inf, "_%f", &tokval.flts)', 'qbe_scanfloat(inf, &tokval.flts, 0)')
        source = source.replace('fscanf(inf, "_%lf", &tokval.fltd)', 'qbe_scanfloat(inf, &tokval.fltd, 1)')
        source = source.replace('fgetc(inf)', 'getc(inf)')
        if source.count('err("too many instructions");') != 3:
            raise RuntimeError('QBE parser scratch guards changed')
        source = source.replace('err("too many instructions");', 'qbe_scratch_exhausted();')
    elif name == 'simpl.c':
        source = replace_once(source, '\t\t\tcuri -= ni;',
                              '\t\t\tif (ni > NIns)\n\t\t\t\tqbe_scratch_exhausted();\n\t\t\tcuri -= ni;')
    elif name == 'emit.c':
        # Recorded RED: test_nested_array_designated_preservation. Equivalent
        # row initializers avoid overwriting sibling nested designators.
        begin = source.index('\tstatic char *sec[2][3] = {')
        end = source.index('\n\t};', begin) + len('\n\t};')
        source = source[:begin] + '''\tstatic char *sec[2][3] = {
        {".text", ".data", ".bss"},
        {".abort \\"unreachable\\"", ".section .tdata,\\"awT\\"", ".section .tbss,\\"awT\\""},
    };''' + source[end:]
    elif name == 'amd64/isel.c':
        # Recorded RED: test_static_compound_literal_pointer. Give the static
        # literal arrays explicit names; preserve each bytecode and its lifetime.
        arrays = []
        def hoist(match):
            label = 'qbe_pattern_' + str(len(arrays))
            arrays.append('static uchar ' + label + '[] = {' + match[1] + '};\n')
            return label
        source = re.sub(r'\(uchar\[\]\)\{([^}]+)\}', hoist, source)
        source = source.replace('static uchar *matcher[] = {', ''.join(arrays) + '\nstatic uchar *matcher[] = {')
        source = split_selector(source)
    source = widen_integers(source)
    source = format_calls(source)
    # qsort callbacks return a 32-bit compiler index, not a 16-bit C int.
    source = re.sub(r'\bqsort\s*\(', 'qbe_qsort(', source)
    # sys/types.h exposes C16 uint, whereas QBE's internal index alias is
    # widened to unsigned long. Namespace QBE tokens without renaming SDK types
    # or changing comments, string literals and emitted assembly.
    source = ''.join('qbe_uint' if token == 'uint' else token for token in TOKEN.findall(source))
    return source


def generate():
    OUTPUT.mkdir(parents=True, exist_ok=True)
    paths = SOURCES + ['all.h', 'ops.h', 'amd64/all.h']
    for name in paths:
        text = adapt(name, (UPSTREAM / name).read_text())
        path = OUTPUT / name
        path.parent.mkdir(parents=True, exist_ok=True)
        if not path.exists() or path.read_text() != text:
            path.write_text(text)
    (OUTPUT / 'config.h').write_text('#define Deftgt T_amd64_sysv\n')
    return [OUTPUT / name for name in SOURCES]


if __name__ == '__main__':
    generate()
