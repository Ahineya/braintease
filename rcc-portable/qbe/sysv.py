"""Separate result, register and stack emission for the pinned SysV call ABI."""


def split_sysv_call(source):
    begin = source.index('static void\nselcall(Fn *fn, Ins *i0, Ins *i1, RAlloc **rap)')
    end = source.index('\nstatic int\nselpar(', begin)
    function = source[begin:end]
    result_at = function.index('\tif (!req(i1->arg[1], R)) {\n\t\tif (aret.inmem)')
    call_at = function.index('\n\temit(Ocall,', result_at)
    registers_at = function.index('\tni = ns = 0;', call_at)
    stack_at = function.index('\n\tif (!stk)\n\t\treturn;', registers_at)
    result = function[result_at:call_at].replace('aret.', 'aret->').replace('retr(reg, &aret)', 'retr(reg, aret)')
    helpers = '''static RAlloc *
qbe_call_result(Fn *fn, Ins *i1, RAlloc **rap, AClass *aret, int *pca)
{
    Ref r, r1, reg[2];
    RAlloc *ra;
    int al, ca = *pca;
''' + result + '\n\t*pca = ca;\n\treturn ra;\n}\n\n'
    registers = function[registers_at:stack_at].replace('aret.inmem', 'aret->inmem')
    helpers += '''static void
qbe_call_registers(Fn *fn, Ins *i0, Ins *i1, AClass *ac, RAlloc *ra, AClass *aret)
{
    Ins *i;
    AClass *a;
    int ni, ns;
    Ref r, r1, r2;
''' + registers + '\n}\n\n'
    stack = function[stack_at:]
    if not stack.endswith('\n}\n'):
        raise ValueError('QBE SysV call body contract changed')
    helpers += '''static void
qbe_call_stack(Fn *fn, Ins *i0, Ins *i1, AClass *ac, uint stk)
{
    Ins *i;
    AClass *a;
    uint off;
    Ref r, r1;
''' + stack + '\n'
    wrapper = function[:result_at]
    wrapper = wrapper.replace('\tIns *i;\n', '').replace('\tint ca, ni, ns, al;', '\tint ca;')
    wrapper = wrapper.replace('\tuint stk, off;', '\tuint stk;').replace('\tRef r, r1, r2, reg[2], env;', '\tRef r, env;')
    wrapper += '\tra = qbe_call_result(fn, i1, rap, &aret, &ca);\n'
    wrapper += function[call_at:registers_at]
    wrapper += '\tqbe_call_registers(fn, i0, i1, ac, ra, &aret);\n\tqbe_call_stack(fn, i0, i1, ac, stk);\n}\n'
    return source[:begin] + helpers + wrapper + source[end:]
