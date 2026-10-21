"""Keep the pinned allocator's existing phases in separate C activations."""


def split_registers(source):
    begin = source.index('void\nrega(Fn *fn)\n{')
    body = source.index('\t/* 1. setup */', begin)
    prefix = source[begin:body]
    declarations, capacity = prefix.split('\t/* Include spilled phi/edge copies, not only hardware registers. */', 1)
    declarations = declarations.split('{\n', 1)[1]
    declarations = declarations.replace('RMap *end, *beg, cur, old, *m;', 'RMap *end, *beg, *cur, *old, *m;')
    markers = ['\t/* 1. setup */', '\t/* 2. assign registers */',
               '\t/* 3. emit copies shared by multiple edges',
               "\tif (debug['R'])  {", '\t/* 4. emit remaining copies in new blocks */',
               "\tif (debug['R']) {\n\t\tfprintf(stderr, \"\\n> Register allocation statistics:"]
    positions = [source.index(marker, body) for marker in markers]
    if positions != sorted(positions) or not source.endswith('\n}\n'):
        raise ValueError('QBE register allocation phase contract changed')
    positions.append(len(source) - len('}\n'))
    names = ['setup', 'assign', 'shared', 'mappings', 'edges', 'statistics']
    fields = ['blk', 'end', 'beg', 'cur', 'old']
    helpers = '''struct QbeRegisters {
    Blk **blk;
    RMap *end, *beg, *cur, *old;
};

'''
    for index, name in enumerate(names):
        load = ''.join('\t' + f + ' = c->' + f + ';\n' for f in fields)
        save = ''.join('\tc->' + f + ' = ' + f + ';\n' for f in fields)
        helpers += 'static void\nqbe_registers_' + name + '(Fn *fn, struct QbeRegisters *c)\n{\n'
        phase = source[positions[index]:positions[index + 1]]
        for field in ['cur', 'old']:
            phase = phase.replace('&' + field, field).replace(field + '.', field + '->')
        if index == 0:
            # Avoid copying two complete mapping arrays at every phase boundary.
            # alloc() supplies the same function lifetime and zeroed storage.
            phase = '\tcur = alloc(sizeof *cur);\n\told = alloc(sizeof *old);\n' + phase
        helpers += declarations + load + phase + save + '}\n\n'
    wrapper = 'void\nrega(Fn *fn)\n{\n\tstruct QbeRegisters c = {0};\n'
    wrapper += '\t/* Include spilled phi/edge copies, not only hardware registers. */' + capacity
    wrapper += ''.join('\tqbe_registers_' + name + '(fn, &c);\n' for name in names) + '}\n'
    return source[:begin] + helpers + wrapper
