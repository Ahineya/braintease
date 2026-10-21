#!/usr/bin/env python3
"""Macro/directive regressions, Clang differential checks and C16 bootstrap."""
from pathlib import Path
import re
import resource
import subprocess
import sys
import tempfile
from fuse_x import INCLUDES

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
CC = HERE / 'build/rcc-c'


def run(args, *, text=None, success=True, fd_limit=None):
    def limit_files():
        soft, hard = resource.getrlimit(resource.RLIMIT_NOFILE)
        resource.setrlimit(resource.RLIMIT_NOFILE, (min(soft, fd_limit), hard))
    p = subprocess.run(list(map(str, args)), input=text, capture_output=True, text=True, timeout=30,
                       preexec_fn=limit_files if fd_limit is not None else None)
    assert (p.returncode == 0) == success, (args, p.stdout, p.stderr)
    return p


def tokens(text):
    text = re.sub(r'^\s*#(?:line)?[^\n]*', '', text, flags=re.M)
    return re.findall(r'''(?:u8|u|U|L)?"(?:\\.|[^"\\])*"|(?:u8|u|U|L)?'(?:\\.|[^'\\])*'|\S+''', text)


# Compare after feeding Clang's output through our scanner too. This avoids
# treating formatting differences as token changes, including quoted spaces.
CASES = [
    '#define A 1 + \\\r\n2\r\nA\r\n',
    '#define A 7\rA\r',
    '#define A B\n#define B 9\nA A_suffix "A B" \'A\'\n',
    '#define I(x) ((x)+1)\n#define W(x) I(x)\nW(I(2))\n',
    '#define W(x) x\n#define A 5\nW(W(W(A)))\n',
    '#define F(x) G(x)\n#define G(x) F(x)\nF(4) G(3)\n',
    '#define F F\nF F\n',
    '#define F G\n#define G(x) x+x\nF\n(8)\n',
    '#define F(x) x\nF (7) F token\n',
    '#define EMPTY\n#define ZERO() 3\nZERO() EMPTY\n',
    '#define S(x) #x\n#define BAD(a,b) a\nS(BAD(1)) S(a\nb) S("a\\\\b")\n',
    '#define JOIN(a,b) a##b\n#define SECOND 9\nJOIN(SEC,OND) JOIN(,foo) JOIN(bar,) JOIN(,)\n',
    '#define JOIN(a,b,c) a##b##c\nJOIN(x,,y) JOIN(,,z) JOIN(,,)\n',
    '#define A foo\n#define JOIN(a,b) a##b\n#define CALL(a,b) JOIN(a,b)\nJOIN(A,x) CALL(A,x)\n',
    '#define V(a,...) a + __VA_ARGS__\nV(1,2,3) V(7,)\n',
    '#define S(...) #__VA_ARGS__\nS(a,b,c) S()\n',
    '#define BAD(a,b) a\n#define IGNORE(x) 1\nIGNORE(BAD(1))\n',
    '#define P +\nP+1 / * ptr\n',
    '#if 0\n#define BAD(a b) garbage\n#include "missing.h"\n#if broken(\nx\n#endif\n#else\nright\n#endif\n',
    '#if 1\none\n#elif 1/0\ntwo\n#else\nthree\n#endif\n',
    '#define A 3\n#if defined A && defined(A) && !defined B && A==3\nyes\n#endif\n',
    '#if 1 || (1/0)\nyes\n#endif\n#if 0 && (1/0)\nwrong\n#endif\n',
    '#if (0 ? 1/0 : 7)==7 && (1 ? 3 : 1/0)==3\nyes\n#endif\n',
    '#if -1 < 1U || (1 ? -1 : 1U)<0\nwrong\n#else\nyes\n#endif\n',
    '#if (0x8000000000000000ULL >> 63)==1 && (-8 >> 2)==-2 && (-9%4)==-1\nyes\n#endif\n',
    '#if 2+3*4==14 && (1<<5)==32 && (17^3)==18 && (~0U & 7)==7\nyes\n#endif\n',
    "#if '\\n'==10 && '\\x41'=='A' && '\\101'=='A'\nyes\n#endif\n",
    '#define A 1/**/+2\n#if A==3 // comment\nyes\n#endif\n',
    '#define A 1 + \\\n2\nA\n',
    '#define A 1\n#undef A\n#ifndef A\nyes\n#endif\n',
    '#line 77 "virtual.c"\n__LINE__ __FILE__\n#define L __LINE__\nL\n',
    '#define x 3\n#define f(a) f(x*(a))\n#undef x\n#define x 2\n#define g f\n#define z z[0]\n#define h g(~\n#define m(a) a(w)\n#define w 0,1\n#define t(a) a\nf(y+1) + f(f(z)) % t(t(g)(0) + t)(1);\ng(x+(3,4)-w) | h 5) & m\n(f)^m(m);\n',
]


def main():
    run([sys.executable, HERE / 'build.py'])
    with tempfile.TemporaryDirectory(prefix='portable-cpp-') as temp:
        directory = Path(temp)
        for i, source in enumerate(CASES):
            path = directory / f'case{i}.c'
            path.write_text(source)
            actual = run([CC, '-E', path]).stdout
            reference = run(['clang', '-E', '-P', '-undef', '-std=c11', path]).stdout
            canonical = run([CC, '-E'], text=reference).stdout
            assert tokens(actual) == tokens(canonical), (i, source, actual, canonical)
        print(f'{len(CASES)} macro/conditional cases agree with Clang', flush=True)
        deep = ROOT / 'c-test/tests/bugs/test_preprocessor_deep_includes.c'
        actual = run([CC, '-E', deep], fd_limit=12).stdout
        reference = run(['clang', '-E', '-P', '-undef', '-std=c11', deep]).stdout
        assert tokens(actual) == tokens(run([CC, '-E'], text=reference).stdout)
        print('32 nested includes agree with Clang with only 12 file descriptors', flush=True)
        local = directory / 'local'
        search = directory / 'search'
        local.mkdir(); search.mkdir()
        (local / 'value.h').write_text('#ifndef VALUE_H\n#define VALUE_H\n#define VALUE 42\n#endif\n')
        (search / 'value.h').write_text('#define SEARCH 7\n')
        (local / 'once.h').write_text('#pragma once\nonce_marker\n')
        (local / 'main.c').write_text('#line 10 "virtual.c"\n#include "value.h"\n#include "value.h"\n#include <value.h>\n#define HEADER "once.h"\n#include HEADER\n#include HEADER\nVALUE SEARCH __LINE__ __FILE__\n')
        actual = run([CC, '-E', '-I', search, local / 'main.c']).stdout
        assert tokens(actual) == ['once_marker','42','7','16','"virtual.c"'], actual
        actual = run([CC, '-E', '-DX=2', '-UX', '-DX=5', '-D', 'F(a)=a+a'], text='X F(3) __STDC__ __STDC_VERSION__\n').stdout
        assert tokens(actual) == ['5','3','+','3','1','201112L'], actual
        cli=directory/'cli.c'
        cli.write_text('#if X != 5\n#error command line macro order\n#endif\nint main(void){return 0;}\n')
        run([sys.executable,HERE/'cc.py',cli,'-DX=2','-UX','-DX=5','--emit-ir'])
        result=run([CC,'-E'],text='#define A 1\n#define A 2\nA\n')
        assert tokens(result.stdout)==['2'] and 'warning: redefinition' in result.stderr,result
        result=run([CC,'-E'],text="#if '\\xffff'==-1 && '\\x8000'==-32768 && 'é'==233\nyes\n#endif\n")
        assert tokens(result.stdout)==['yes'],result
        for code, message in [
            ('#if 1\nx\n','unterminated conditional'),
            ('#else\n','unmatched conditional'),
            ('#if 1\n#else\n#else\n#endif\n','conditional after #else'),
            ('#if 1/0\n#endif\n','division by zero'),
            ('#if 1 << 64\n#endif\n','invalid shift'),
            ('#if defined()\n#endif\n','defined requires'),
            ('#include "missing.h"\n','cannot find include'),
            ('#define F(a,b) a\nF(1)\n','wrong number'),
            ('#define F(a) a\nF(1\n','unterminated arguments'),
            ('#define J(a,b) a##b\nJ(+,*)\n','invalid token paste'),
            ('#define J(a) ##a\n','cannot start or end'),
            ("#if '\\x10000'\n#endif\n",'escape is out of range'),
            ('#error deliberate\n','#error: deliberate'),
        ]:
            result = run([CC, '-E'], text=code, success=False)
            assert message in result.stderr, (code,result.stderr)
        (local / 'bad.h').write_text('#if 1\n')
        (local / 'bad.c').write_text('#include "bad.h"\n#endif\n')
        assert 'unterminated conditional' in run([CC,'-E',local/'bad.c'],success=False).stderr
        (local / 'large.h').write_bytes(b'/*' + b' ' * 35000 + b'*/\r\n#define LARGE(x) x + \\\r\n1\r\n')
        (local / 'large.c').write_text('#include "large.h"\nLARGE(2)\n')
        assert tokens(run([CC, '-E', local / 'large.c']).stdout) == ['2', '+', '1']
        (local / 'zero.h').write_bytes(b'int before;\0int after;\n')
        (local / 'zero.c').write_text('#include "zero.h"\n')
        run([CC, local / 'zero.c'], success=False)
        (local / 'directory.h').mkdir()
        (local / 'directory.c').write_text('#include "directory.h"\n')
        assert 'cannot read include' in run([CC, '-E', local / 'directory.c'], success=False).stderr
        print('header lookup, guards, pragma once, line tracking, CLI definitions, C16 characters and diagnostics passed',flush=True)
        # Compile all compiler translation units directly, using the same C16
        # headers as the guest. No Rust preprocessor participates here.
        options=['-I',HERE/'guest/include','-I',HERE/'build/frontend']
        for include in INCLUDES:options+=['-I',include]
        sources=sorted((HERE/'build/frontend').glob('*.c'))+sorted((HERE/'src').glob('*.c'))
        for path in sources:
            direct=run([CC,*options,path]).stdout
            prepared=directory/'roundtrip.i'
            prepared.write_text(run([CC,'-E',*options,path]).stdout)
            recompiled=run([CC,prepared]).stdout
            assert direct==recompiled,path
        print(f'{len(sources)} compiler translation units compile raw C and match -E round trips',flush=True)


if __name__=='__main__':main()
