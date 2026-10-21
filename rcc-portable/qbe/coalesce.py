"""Keep each phase of upstream stack-slot coalescing in a small activation.

The C16 continuation lowering retains many temporaries in a native frame.
Splitting at existing phase boundaries preserves the algorithm while avoiding
an activation larger than the x86 kernel's guarded stack.
"""


def split_coalesce(source):
    begin = source.index('void\ncoalesce(Fn *fn)\n{')
    body = source.index('\t/* minimize the stack usage', begin)
    declarations = source[begin:body].split('{\n', 1)[1]
    markers = ['\t/* minimize the stack usage', '\t/* one-pass liveness analysis */',
               '\t/* kill dead stores */', '\t/* kill slots with an empty live range */',
               '\t/* fuse slots by decreasing size */', '\t/* substitute fused slots */',
               '\t/* fix newly overlapping blits */', "\tif (debug['M']) {\n\t\tfor (s0=sl;",
               '\tfor (s=sl; s<&sl[nsl]; s++)\n\t\tvfree(s->st);']
    positions = [source.index(marker, body) for marker in markers]
    if positions != sorted(positions) or source[positions[-1]:] != (
            '\tfor (s=sl; s<&sl[nsl]; s++)\n\t\tvfree(s->st);\n\tvfree(sl);\n}\n'):
        raise ValueError('QBE coalescing phase contract changed')
    fields = ['sl', 'bl', 'nsl', 'nbl', 'ip', 'total', 'freed', 'fused']
    context = '''struct QbeCoalesce {
    Slot *sl;
    Ins **bl;
    int nsl, nbl, ip;
    uint total, freed, fused;
};

'''
    helpers = []
    names = ['slots', 'liveness', 'deadstores', 'deadslots', 'fuse', 'substitute', 'blits', 'debug']
    for index, name in enumerate(names):
        phase = source[positions[index]:positions[index + 1]]
        # All inter-phase state is explicit. Ordinary phase-local variables
        # keep their upstream declarations and lifetime inside the helper.
        load = ''.join('\t' + field + ' = c->' + field + ';\n' for field in fields)
        save = ''.join('\tc->' + field + ' = ' + field + ';\n' for field in fields)
        helpers.append('static void\nqbe_coalesce_' + name + '(Fn *fn, struct QbeCoalesce *c)\n{\n'
                       + declarations + load + phase + save + '}\n\n')
    wrapper = 'void\ncoalesce(Fn *fn)\n{\n\tstruct QbeCoalesce c = {0};\n\tSlot *s;\n'
    wrapper += ''.join('\tqbe_coalesce_' + name + '(fn, &c);\n' for name in names)
    wrapper += '\tfor (s=c.sl; s<&c.sl[c.nsl]; s++)\n\t\tvfree(s->st);\n\tvfree(c.sl);\n}\n'
    return source[:begin] + context + ''.join(helpers) + wrapper
