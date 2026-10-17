"""Small, checked source adaptations against the pinned Lynx archive."""
from pathlib import Path
import re
import subprocess


def replace_once(text, before, after):
    if text.count(before) != 1:
        raise RuntimeError(f'Upstream adaptation no longer matches: {before!r}')
    return text.replace(before, after)


def generate(source: Path, output: Path):
    output.mkdir(parents=True, exist_ok=True)
    grid = (source / 'src/GridText.c').read_text()
    # This private type is also the pool allocation unit. Four cells keep its
    # size a multiple of ALIGN_SIZE (sizeof(double), four C16 cells). Direction
    # and position writes already use upstream CAST_DIR/CAST_POS masks; style
    # retains its exact unsigned-16-bit storage type.
    for before, after in [
        ('unsigned sc_direction:BITS_DIR;', 'unsigned sc_direction;'),
        ('unsigned sc_horizpos:BITS_POS;', 'unsigned sc_horizpos;'),
        ('unsigned sc_style:16;', 'unsigned sc_style; unsigned ripple_pool_padding;'),
    ]:
        grid = replace_once(grid, before, after)
    (output / 'GridText.c').write_text(grid)
    sgml = (source / 'WWW/Library/Implementation/SGML.h').read_text()
    # Tag classes have bits through 0x100000; int on RCC has only 16 bits.
    sgml = replace_once(sgml, 'typedef int TagClass;', 'typedef unsigned long TagClass;')
    (output / 'SGML.h').write_text(sgml)
    lex = (source / 'WWW/Library/Implementation/HTLex.c').read_text()
    lex = replace_once(lex, 'sizeof(HTlex_buffer) + 30', 'sizeof(char[40]) + 30')
    (output / 'HTLex.c').write_text(lex)

    # Keep the upstream HTTP/parser code, bounding the wire data before it
    # reaches any MIME/HTML allocation. HTTP retries start a fresh budget.
    http = (source / 'WWW/Library/Implementation/HTTP.c').read_text()
    http = replace_once(http, '#include <HTTP.h>', '#include <HTTP.h>\n#include <ripple_browser.h>')
    http = replace_once(http, '#define HTTP_NETREAD(a, b, c, d)   NETREAD(a, b, c)',
                        '#define HTTP_NETREAD(a, b, c, d)   ripple_browser_read(a, b, c)')
    http = replace_once(http, '#define HTTP_NETWRITE(a, b, c, d)  NETWRITE(a, b, c)',
                        '#define HTTP_NETWRITE(a, b, c, d)  ripple_browser_write(a, b, c)')
    http = replace_once(http, '  try_again:', '  try_again:\n    ripple_browser_begin();')
    http = replace_once(http, '    rv = HTCopy(anAnchor, s, (void *) handle, target);',
                        '    rv = HTCopy(anAnchor, s, (void *) handle, target);\n'
                        '    if (ripple_browser_limit_hit()) {\n'
                        '        HTTP_NETCLOSE(s, handle);\n'
                        '        status = HT_NOT_LOADED;\n'
                        '        goto clean_up;\n    }')
    (output / 'HTTP.c').write_text(http)
    formats = (source / 'WWW/Library/Implementation/HTFormat.c').read_text()
    formats = replace_once(formats, '#include <HTTP.h>', '#include <HTTP.h>\n#include <ripple_browser.h>')
    formats = formats.replace('NETREAD(file_number, input_buffer, INPUT_BUFFER_SIZE)',
                              'ripple_browser_read(file_number, input_buffer, INPUT_BUFFER_SIZE)')
    (output / 'HTFormat.c').write_text(formats)

    # The shared curses port resizes stdscr itself. Lynx still needs to sample
    # the new dimensions, without claiming the unavailable ncurses wresize.
    screen = (source / 'src/LYCurses.h').read_text()
    screen = replace_once(screen, '#if defined(USE_CURSES_RESIZE) || defined(USE_SLANG)',
                          '#if defined(USE_CURSES_RESIZE) || defined(USE_SLANG) || defined(RIPPLE_MMU)')
    (output / 'LYCurses.h').write_text(screen)
    screen = (source / 'src/LYCurses.c').read_text()
    screen = replace_once(screen, '#include <HTUtils.h>', '#include <HTUtils.h>\n#include <ripple_browser.h>')
    if screen.count('initscr();') != 2:
        raise RuntimeError('Upstream screen initialization changed')
    screen = screen.replace('initscr();', 'ripple_browser_screen_init();')
    (output / 'LYCurses.c').write_text(screen)

    main = (source / 'src/LYMain.c').read_text()
    main = replace_once(main, '#include <HTUtils.h>', '#include <HTUtils.h>\n#include <ripple_browser.h>')
    main = replace_once(main, 'int main(int argc,\n\t char **argv)\n{',
                        'int main(int argc,\n\t char **argv)\n{\n    ripple_browser_init();')
    (output / 'LYMain.c').write_text(main)


def initializer_count(text, name):
    """Count top-level entries in an already preprocessed structure array."""
    match = re.search(r'\b' + re.escape(name) + r'\s*\[\s*\]\s*=\s*\{', text)
    if not match:
        raise RuntimeError(f'Cannot find initializer: {name}')
    tokens = re.finditer(r'"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|[^\s]', text[match.end():])
    depth, entries, populated = 1, 0, False
    for token in tokens:
        value = token.group()
        if value == '}' and depth == 1:
            return entries + int(populated)
        if value == ',' and depth == 1:
            entries += 1
            populated = False
            continue
        if depth == 1:
            populated = True
        if value == '{': depth += 1
        if value == '}': depth -= 1
    raise RuntimeError(f'Unterminated initializer: {name}')


def array_bounds(source, output, preprocessor, flags):
    # RCC currently cannot fold sizeof(array_object) in a declarator. Derive
    # only the two affected array lengths from the preprocessed target tables,
    # under exactly the same feature flags; never substitute host sizeof.
    for unit, table, before, declaration in [
        ('LYEditmap', 'LYLineEditors', 'const char *LYEditorNames[TABLESIZE(LYLineEditors) + 1];',
         'const char *LYEditorNames[{count} + 1];'),
        ('LYReadCFG', 'Config_Table', 'typedef BOOL (optidx_set_t)[NOPTS_];',
         'typedef BOOL optidx_set_t[{count} - 1];'),
    ]:
        original = source / 'src' / (unit + '.c')
        run = subprocess.run([*preprocessor, *flags, str(original)], capture_output=True, text=True, check=True)
        count = initializer_count(run.stdout, table)
        if not count: raise RuntimeError(f'Empty table: {table}')
        text = replace_once(original.read_text(), before, declaration.format(count=count))
        (output / (unit + '.c')).write_text(text)
