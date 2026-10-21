"""Factor the pinned amd64 selector into case-sized C activations.

The portable backend currently gives the original switch a 138 KiB native
frame. Ordinary calls between these case handlers keep the guarded kernel
stack unchanged; instruction emission order and the selector algorithm stay
identical. Upstream source is never edited.
"""
import re


def split_selector(source):
    start = source.index('static void\nsel(Ins i, Num *tn, Fn *fn)')
    finish = source.index('\nstatic Ins *\nflagi(', start)
    function = source[start:finish]
    body_at = function.index('\tswitch (i.op) {')
    tail_at = function.index('\n\twhile (i0>curi && --i0)')
    switch = function[body_at:tail_at]
    if not switch.endswith('\n\t}\n'):
        raise ValueError('QBE selector switch contract changed')
    switch = switch[:-len('\n\t}\n')]
    markers = ['\tcase Odiv:', '\tcase Osar:', '\tcase Ouwtof:', '\tcase Oultof:',
               '\tcase Ostoui:', '\tcase Onop:', '\tcase Ostored:', '\tcase_Oload:',
               '\tcase Odbgloc:', '\tcase Oalloc4:', '\tdefault:']
    positions = [switch.index(marker) for marker in markers]
    if positions != sorted(positions):
        raise ValueError('QBE selector case ordering changed')
    chunks = [switch[positions[i]:positions[i + 1] if i + 1 < len(positions) else len(switch)]
              for i in range(len(positions))]

    def leading_cases(chunk):
        lines = chunk.splitlines(keepends=True)
        at = 0
        while at < len(lines) and lines[at].startswith('\tcase '):
            at += 1
        if not at:
            raise ValueError('expected selector opcode labels')
        return ''.join(lines[:at]), ''.join(lines[at:])

    locals_by_kind = [('Ref', ['r0', 'r1', 'tmp[7]']),
                      ('int', ['x', 'j', 'k', 'kc', 'sh', 'swap']), ('Ins *', ['i0', 'i1'])]

    def helper(name, body):
        body = body.replace('goto Emit;', '{ qbe_sel_plain(i, tn, fn); return; }')
        body = body.replace('goto case_Oext;', '{ qbe_sel_plain(i, tn, fn); return; }')
        body = body.replace('goto case_Oxsel;', '{ qbe_sel_plain(i, tn, fn); return; }')
        body = body.replace('goto case_Oload;', '{ qbe_sel_load(i, tn, fn); return; }')
        code = re.sub(r'/\*[\s\S]*?\*/|"(?:\\.|[^"\\])*"', '', body)
        identifiers = set(re.findall(r'\b[A-Za-z_]\w*\b', code))
        declarations = []
        for kind, candidates in locals_by_kind:
            used = [v for v in candidates if v.split('[')[0] in identifiers]
            if used:
                declarations.append('\t' + kind + ' ' + ', '.join(used) + ';\n')
        if 'k' in identifiers:
            declarations.append('\tk = i.cls;\n')
        return ('static void\nqbe_sel_' + name + '(Ins i, Num *tn, Fn *fn)\n{\n'
                + ''.join(declarations) + '\tdo {\n' + body + '\t} while (0);\n}\n\n')

    # The shared Emit label needs its own handler before any case can call it.
    plain = chunks[8]
    emit_at = plain.index('Emit:\n')
    plain_cases = plain[:emit_at]
    plain_cases = plain_cases.replace('\tcase_Oxsel:\n', '').replace('\tcase_Oext:\n', '')
    helpers = 'static void sel(Ins, Num *, Fn *);\n\n'
    helpers += helper('plain', plain[emit_at + len('Emit:\n'):])
    helpers += helper('load', chunks[7][len('\tcase_Oload:\n'):])
    dispatch = []
    names = {0: 'division', 1: 'shift', 2: 'uwtof', 3: 'ultof', 6: 'store', 9: 'alloc'}
    for index, chunk in enumerate(chunks):
        if index in names:
            cases, body = leading_cases(chunk)
            name = names[index]
            helpers += helper(name, body)
        elif index == 4:
            name = 'ftoui'
            cases = '\tcase Ostoui:\n\tcase Odtoui:\n'
            prefix = '''
        if (i.op == Ostoui) {
            i.op = Ostosi;
            kc = Ks;
            tmp[4] = getcon(0xdf000000, fn);
        } else {
            i.op = Odtosi;
            kc = Kd;
            tmp[4] = getcon(0xc3e0000000000000, fn);
        }
'''
            body = prefix + chunk[chunk.index('\tOftoui:\n') + len('\tOftoui:\n'):]
            helpers += helper(name, body)
        elif index == 5:
            dispatch.append('\tcase Onop:\n\t\tbreak;\n')
            continue
        elif index == 7:
            continue
        elif index == 8:
            name, cases = 'plain', plain_cases
        elif index == 10:
            name, cases = 'other', '\tdefault:\n'
            helpers += helper(name, chunk[len('\tdefault:\n'):])
        else:
            raise ValueError('unhandled selector case')
        dispatch.append(cases + '\t\tqbe_sel_' + name + '(i, tn, fn);\n\t\tbreak;\n')
    head = function[:body_at]
    head = head.replace('\tRef r0, r1, tmp[7];\n\tint x, j, k, kc, sh, swap;\n\tIns *i0, *i1;', '\tIns *i0;')
    head = head.replace('\tk = i.cls;\n', '')
    result = helpers + head + '\tswitch (i.op) {\n' + ''.join(dispatch) + '\t}\n' + function[tail_at:]
    return source[:start] + result + source[finish:]
