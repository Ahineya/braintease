#!/usr/bin/env python3
"""Generate C16 frontend sources without modifying the pinned upstream copy."""
from pathlib import Path
import re

HERE = Path(__file__).resolve().parent
SOURCES = 'attr decl eval expr init map pp scan scope stmt token tree type utf util'.split()


def write_changed(path, source):
    # Bootstrap builds also compile these generated files. Avoid rewriting an
    # unchanged snapshot while another tool is reading it.
    if not path.exists() or path.read_text() != source:
        path.write_text(source)


def adapt(name, source):
    if name == 'pp':
        return (HERE / 'preprocessor.c').read_text()
    if name == 'scan':
        source = source.replace('FILE *file;', 'FILE *file;\n\tconst char *text;\n\tsize_t pos;')
        source = source.replace('static void\nnextchar', '''static int
inputget(struct scanner *s)
{
    if (!s->text) return getc(s->file);
    return s->text[s->pos] ? (unsigned char)s->text[s->pos++] : EOF;
}

static void
inputback(struct scanner *s, int c)
{
    if (c == EOF) return;
    if (s->text) --s->pos;
    else ungetc(c, s->file);
}

static void
nextchar''')
        source = source.replace('getc(s->file)', 'inputget(s)').replace('ungetc(c, s->file)', 'inputback(s, c)').replace('ungetc(s->chr, s->file)', 'inputback(s, s->chr)')
        # Keep the actual file operations inside the input abstraction.
        source = source.replace('if (!s->text) return inputget(s);', 'if (!s->text) return getc(s->file);').replace('else inputback(s, c);', 'else ungetc(c, s->file);')
        source = source.replace('s->file = file;', 's->file = file;\n\ts->text = NULL;\n\ts->pos = 0;\n\ts->chr = 0;\n\ts->sawspace = false;')
        source = source.replace('static void\nscanclose(void)', 'static void\nscanclose(void)')
        source = source.replace('fclose(scanner->file);', 'if (scanner->file && scanner->file != stdin) fclose(scanner->file);')
        begin = source.index('\tscanner->sawspace = false;\n\tfor (;;) {', source.index('void\nscan(struct token *t)'))
        end = source.index('\tif (scanner->usebuf)', begin)
        source = source[:begin] + '\tscanner->sawspace = false;\n\tt->kind = scankind(scanner, &t->loc);\n\tt->pphide = NULL;\n' + source[end:]
        source += '''
void scanstring(const char *text, struct location loc)
{
    scanfrom(loc.file, NULL);
    scanner->text = text;
    scanner->loc = loc;
    if (scanner->loc.col) --scanner->loc.col;
    nextchar(scanner);
}

bool scanpop(void)
{
    struct scanner *previous = scanner->next;
    scanclose();
    scanner = previous;
    return scanner != NULL;
}
'''
        return source
    if name == 'type':
        source = source.replace('n * 8', 'n * 16')
        for kind, size in [('TYPESHORT', 1), ('TYPEINT', 1), ('TYPELONG', 2), ('TYPELLONG', 4)]:
            source = re.sub(r'INTTYPE\(' + kind + r', \d+,', f'INTTYPE({kind}, {size},', source)
        for kind, size in [('TYPEFLOAT', 2), ('TYPEDOUBLE', 4), ('TYPELDOUBLE', 4)]:
            source = re.sub(r'FLTTYPE\(' + kind + r', \d+\)', f'FLTTYPE({kind}, {size})', source)
        source = source.replace('.kind = TYPENULLPTR, .size = 8, .align = 8', '.kind = TYPENULLPTR, .size = 2, .align = 1')
        source = source.replace('.align = n,', '.align = 1,')
        source = source.replace('t->size = 8;\n\tt->align = 8;', 't->size = 2;\n\tt->align = 1;')
        source = source.replace('t->align = t->size;', 't->align = 1;')
        source = source.replace('t->size * 8', 't->size * 16').replace('typeint.size * 8', 'typeint.size * 16')
        source = source.replace('t->size << 3', 't->size << 4')
        source = source.replace('(8 - t->size << 3)', '(64 - t->u.arith.width)')
        source = source.replace('(8 - t->size << 4)', '(64 - t->u.arith.width)')
        for old, new in [('0x081', '0x101'), ('0x101', '0x102'), ('0x201', '0x103'), ('0x402', '0x405')]:
            # Match whole return statements so the substitutions cannot cascade.
            kind = {'0x081':'TYPECHAR', '0x101':'TYPESHORT', '0x201':'TYPEINT', '0x402':'TYPELLONG'}[old]
            source = re.sub(r'(case '+kind+r':\s+return )'+old, r'\g<1>'+new, source)
        source = source.replace('return t->size << 4 + 4 | 0x1;', 'return t->u.arith.width << 4 | 4;')
        source = source.replace('case TYPELLONG: return &typellong;', 'case TYPELLONG: return &typeullong;')
        source = source.replace('if (t == &typefloat)\n', 'if (t == &typebool) return &typeint;\n\tif (t == &typefloat)\n')
    elif name == 'decl':
        source = source.replace('* 8', '* 16').replace('(width - b->bits + 7) / 8', '(width - b->bits + 15) / 16').replace('% 8', '% 16')
        source = source.replace('if (align < mt.type->align)', 'if (align > 1) error(&tok.loc, "C16 extended member alignment is not implemented");\n\t\tif (align < mt.type->align)')
        source = source.replace('mapkey(&key, expr->u.string.data, expr->u.string.size);', 'mapkey(&key, expr->u.string.data, expr->u.string.size * sizeof(uint_least16_t));')
        source = source.replace('#include <stdbool.h>', '#include <stdbool.h>\n#include <stdint.h>')
    elif name == 'expr':
        source = source.replace('e->type->size * 8', 'e->type->size * 16')
        source = source.replace('(val ^ 0x80) - 0x80', '(val ^ 0x8000) - 0x8000')
        source = source.replace('case TCHARCONST:\n\t\tsrc = tok.lit;', 'case TCHARCONST:\n\t\tif (tok.lit[0]!=\'\\\'\') error(&tok.loc, "portable IR: prefixed character encodings are not implemented");\n\t\tsrc = tok.lit;')
        source = source.replace('if (forceutf8)\n\t\tkind', 'if (!forceutf8 && kind) error(&tok.loc, "portable IR: prefixed string encodings are not implemented");\n\tif (forceutf8)\n\t\tkind')
        source = source.replace('e->toeval = NULL;', 'e->toeval = NULL;\n\te->pointer_difference = false;')
        source = source.replace('e = mkbinaryexpr(loc, TSUB, exprconvert(l, &typelong), exprconvert(r, &typelong));',
                                'e = mkbinaryexpr(loc, TSUB, exprconvert(l, &typelong), exprconvert(r, &typelong));\n\t\t\te->pointer_difference = true;')
        # Target string cells and host storage octets are different units.
        source = source.replace('switch (t->size) {\n\tcase 1:', 'switch (forceutf8 ? 0 : t->size) {\n\tcase 0:')
        source = source.replace('\tcase 2:\n\t\twidth = sizeof(uint_least16_t);', '\tcase 1:\n\t\twidth = sizeof(uint_least16_t);')
        source = source.replace('\tcase 4:\n\t\twidth = sizeof(uint_least32_t);', '\tcase 2:\n\t\twidth = sizeof(uint_least32_t);')
    elif name == 'eval':
        source = source.replace('t->size == 4', 't->kind == TYPEFLOAT')
    elif name == 'init':
        source = source.replace('* 8', '* 16')
    elif name == 'tree':
        source = source.replace('sizeof(void *) * 8', 'sizeof(void *) * CHAR_BIT')
        source = '#include <limits.h>\n' + source
    return source


def main():
    destination = HERE / 'build/frontend'
    destination.mkdir(parents=True, exist_ok=True)
    for name in SOURCES:
        write_changed(destination / f'{name}.c', adapt(name, (HERE / f'upstream/cproc/{name}.c').read_text()))
    for header in (HERE / 'upstream/cproc').glob('*.h'):
        source = header.read_text()
        if header.name == 'cc.h':
            source = source.replace('struct expr {\n', 'struct expr {\n\tbool pointer_difference;\n')
            source = source.replace('struct token {\n', 'struct token {\n\tstruct pphide *pphide;\n')
            source = source.replace('void scanopen(void);', 'void scanopen(void);\nvoid scanstring(const char *, struct location);\nbool scanpop(void);')
            source = source.replace('void ppinit(void);', 'void ppinit(void);\nvoid ppinclude(const char *);\nvoid ppdefine(const char *);\nvoid ppundef(const char *);\nvoid ppoutput(FILE *);')
        elif header.name == 'util.h':
            source = source.replace('#if __STDC_VERSION__ >= 202311L',
                                    '#if defined(__GNUC__) || defined(__clang__)\n#define noreturn __attribute__((noreturn))\n#elif __STDC_VERSION__ >= 202311L')
        write_changed(destination / header.name, source)


if __name__ == '__main__':
    main()
