#!/usr/bin/env python3
"""Generate checked C16 working sources without modifying upstream files."""
import hashlib
import json
from pathlib import Path
import re
import subprocess
import os
import tempfile
import fcntl

PORT = Path(__file__).resolve().parent


def write_changed(path, data):
    path.parent.mkdir(parents=True, exist_ok=True)
    if path.exists() and path.read_bytes() == data:
        return
    with tempfile.NamedTemporaryFile(dir=path.parent, delete=False) as output:
        output.write(data)
        temporary = output.name
    os.replace(temporary, path)


def replace(source, old, new, count=1):
    if source.count(old) != count:
        raise ValueError(f'Upstream adaptation no longer matches: {old!r}')
    return source.replace(old, new)


def adapt(name, source):
    # Port-local spelling makes addressable cells explicit throughout the
    # dependency graph. Do not lie about uint8_t in the common C library.
    source = re.sub(r'\buint8_t\b', 'ns_octet_t', source)
    source = source.replace('#include <inttypes.h>', '#include <netsurf_c16.h>')
    source = source.replace('#include <stdint.h>', '#include <netsurf_c16.h>')
    if name == 'libparserutils/src/charset/codec.c':
        # UTF-16's host-word casts have not been ported yet. Reject the
        # encoding through the normal BADENCODING result, never misdecode it.
        source = replace(source, '\t&charset_utf16_codec_handler,\n', '')
    if name.startswith('libparserutils/src/charset/codecs/codec_') and name.endswith('.c') and not name.endswith('utf16.c'):
        source = replace(source,
            'endian_big_to_host(*((uint32_t *) (void *) *source))',
            'ns_read_be32(*source)')
        source = replace(source,
            '*((uint32_t *) (void *) *dest) =\n\t\t\t\t\tendian_host_to_big(pread[0]);',
            'ns_write_be32(*dest, pread[0]);')
        source = replace(source,
            '*((uint32_t *) (void *) *dest) = endian_host_to_big(ucs4);',
            'ns_write_be32(*dest, ucs4);')
        # Reject incomplete internal UCS-4 words rather than reading past
        # their buffer. Normal callers supply complete four-octet words.
        source = replace(source, 'while (*sourcelen > 0) {\n\t\tucs4',
            'while (*sourcelen > 0) {\n\t\tif (*sourcelen < 4) return PARSERUTILS_NEEDDATA;\n\t\tucs4')
    if name == 'libparserutils/src/input/filter.c':
        source = replace(source, 'uint32_t pivot_buf[64];', 'ns_octet_t pivot_buf[64 * 4];')
    if name == 'libparserutils/src/charset/encodings/utf8impl.h':
        source = replace(source, '~((1 << (8 - l)) - 1) | ucs4',
            '(~((1 << (8 - l)) - 1) | ucs4) & 255')
        source = replace(source, 'int l = 0;', 'size_t l = 0;')
    if name == 'libhubbub/src/treebuilder/in_body.c':
        # RCC currently mistakes a declarator that hides a typedef for a
        # second type specifier. Correct-behavior red regression:
        # c-test/tests/bugs/test_typedef_hidden_parameter.c.
        source = replace(source, '} bookmark;', '} ns_bookmark_type;')
        source = replace(source, 'bookmark *bookmark', 'ns_bookmark_type *bookmark', 2)
        source = replace(source, 'bookmark bookmark;', 'ns_bookmark_type bookmark;')
    if name == 'libcss/include/libcss/fpmath.h':
        source = re.sub(r'\bINT_MAX\b', 'INT32_MAX', source)
        source = re.sub(r'\bINT_MIN\b', 'INT32_MIN', source)
        source = replace(source, 'css_int_to_fixed(const int a)', 'css_int_to_fixed(const int32_t a)')
        # The upstream cmov tricks overflow signed int32_t before checking.
        # Use defined saturation on both our backend and host reference builds.
        for operation, sign in (('add', '+'), ('subtract', '-')):
            start = source.index('static inline css_fixed\ncss_' + operation + '_fixed')
            end = source.index('\nstatic inline css_fixed', start + 1)
            source = source[:start] + (
                f'static inline css_fixed\ncss_{operation}_fixed(const css_fixed x, const css_fixed y) {{\n'
                f'\tint64_t value = (int64_t)x {sign} (int64_t)y;\n'
                '\tif (value > INT32_MAX) return INT32_MAX;\n'
                '\tif (value < INT32_MIN) return INT32_MIN;\n'
                '\treturn (css_fixed)value;\n}\n') + source[end:]
    if name == 'libcss/src/bytecode/bytecode.h':
        # This unused typedef also collides with ordinary local 'unit'
        # variables (the same recorded RCC hiding defect as Hubbub bookmark).
        source = replace(source, '} unit;', '} ns_css_bytecode_unit;')
        source = replace(source, '(flags << 10)', '((uint32_t)flags << 10)')
        source = replace(source, '((value & 0x3fff) << 18)', '(((uint32_t)value & 0x3fff) << 18)')
    if name == 'libcss/src/select/bloom.h':
        # C99 array-parameter qualifiers are recorded in the red regression
        # test_restrict_array_parameter. Removing this aliasing promise does
        # not change program results.
        source = replace(source, '[restrict CSS_BLOOM_SIZE]', '[CSS_BLOOM_SIZE]', 2)
        source = replace(source, '(1 << bit)', '(1UL << bit)', 2)
    if name == 'libcss/src/select/select.c':
        # One more occurrence of the typedef-hiding parser defect.
        source = replace(source, 'prop_state *prop_state,', 'prop_state *property_state,', 2)
        source = replace(source, '*prop_state = select_state', '*property_state = select_state')
        source = replace(source, 'prop_state->', 'property_state->', 4)
        source = replace(source, ', prop_state,', ', property_state,', 2)
    if name == 'libcss/src/utils/utils.c':
        # CSS fixed-point bounds must not be evaluated in a 16-bit int.
        for shift in (21, 22):
            source = source.replace(f'(1 << {shift})', f'(1L << {shift})')
    if name == 'libcss/src/parse/properties/utils.c':
        source = replace(source,
            '((unsigned)a << 24) | (r << 16) | (g << 8) | b',
            '((uint32_t)a << 24) | ((uint32_t)r << 16) | ((uint32_t)g << 8) | b', 2)
    if name.startswith('libcss/src/select/autogenerated_'):
        # Small hex masks (notably 0xff00) are unsigned int16 on C16.
        # Their complements must retain the other 16 bits of a packed style.
        source = re.sub(r'(#define \w+_MASK )(0x[0-9a-fA-F]+)\b', r'\1\2UL', source)
    if name in ('libdom/include/dom/core/exceptions.h', 'libdom/bindings/hubbub/errors.h'):
        # Error class flags live above the guest int's 16-bit range.
        source = source.replace('(1<<16)', '(1L<<16)').replace('(1<<17)', '(1L<<17)')
        # C99 enumerators must fit int. Use explicit 32-bit constants and
        # result types instead of relying on a wide-enum compiler extension.
        def wide_constants(match):
            entries, typename = match.groups()
            entries = re.sub(r'/\*.*?\*/', '', entries, flags=re.S)
            definitions = []
            for entry in entries.split(','):
                if entry.strip():
                    key, value = entry.split('=', 1)
                    definitions.append(f'#define {key.strip()} ((uint32_t)({value.strip()}))')
            return '\n'.join(definitions) + f'\ntypedef uint32_t {typename};'
        source = re.sub(r'typedef enum\s*\{(.*?)\}\s*(\w+);', wide_constants, source, flags=re.S)
        source = '#include <netsurf_c16.h>\n' + source
    if name in ('libdom/src/utils/character_valid.h', 'libdom/src/utils/character_valid.c'):
        # Unicode scalar values and XML ranges must retain non-BMP bits.
        source = source.replace('unsigned int ch', 'uint32_t ch')
        source = source.replace('unsigned int start;', 'uint32_t start;')
        source = source.replace('unsigned int end;', 'uint32_t end;')
        if name.endswith('.h'):
            source = replace(source, '#include <stdbool.h>', '#include <stdbool.h>\n#include <netsurf_c16.h>')
    if name == 'libdom/src/html/html_element.c':
        # The guest SDK has no inttypes printf macros. Explicitly match the
        # variadic type on both the C16 guest and the conventional host oracle.
        source = replace(source, '"%"PRIu32, value', '"%lu", (unsigned long)value', 2)
    if name == 'libdom/src/core/string.c':
        # RCC loses nested union member types in static initializers (recorded
        # by test_nested_static_pointer_initializer on both backends). These
        # private alternatives are accessed exclusively through the type tag;
        # they need not overlap. Separate storage costs one pointer per string
        # while retaining the original API and defined C object accesses.
        source = replace(source, '\tunion {\n', '\tstruct {\n')
    if name.startswith(('netsurf/', 'libnsutils/')):
        from core_adapt import adapt_core
        source = adapt_core(name, source)
    return source


def prepare(destination):
    destination = Path(destination).resolve()
    destination.parent.mkdir(parents=True, exist_ok=True)
    with (destination.parent / (destination.name + '.lock')).open('a') as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        return prepare_locked(destination)


def prepare_locked(destination):
    manifest = json.loads((PORT / 'upstream-manifest.json').read_text())
    changed = {}
    for name, checksum in manifest.items():
        original = (PORT / 'upstream' / name).read_bytes()
        if hashlib.sha256(original).hexdigest() != checksum:
            raise ValueError(f'Pinned NetSurf source changed: {name}')
        data = original
        if name.endswith(('.c', '.h')):
            # Historical, unused host frontends contain non-UTF8 comments.
            # Keep these pristine; they are outside this guest source profile.
            if name not in ('netsurf/frontends/amiga/schedule.c',
                            'netsurf/frontends/atari/gemtk/dragdrop.c',
                            'netsurf/frontends/riscos/theme_install.c'):
                data = adapt(name, original.decode()).encode()
        write_changed(destination / name, data)
        if data != original:
            changed[name] = hashlib.sha256(data).hexdigest()
    # Upstream installs its binding headers under dom/bindings. Reproduce
    # that public include layout without installing into the host system.
    for header in ('parser.h', 'errors.h'):
        write_changed(destination / 'libdom/include/dom/bindings/hubbub' / header,
                      (destination / 'libdom/bindings/hubbub' / header).read_bytes())
    write_changed(destination / 'netsurf/testament.h', b'''/* Pinned source archive; reproducible metadata, no host identity. */
#define WT_NO_GIT 1
#define WT_MODIFIED 0
#define WT_MODIFICATIONS {{0,0}}
#define WT_BRANCHPATH "release/3.11"
#define WT_REVID "3.11"
#define WT_COMPILEDATE "reproducible"
#define WT_HOSTNAME "C16"
#define WT_ROOT "NetSurf"
#define USERNAME "NetSurf"
#define GECOS "NetSurf"
''')
    resources = ['/* NetSurf upstream CSS; generated from pinned resources. */']
    for name in ('default', 'quirks', 'adblock', 'internal'):
        css = (destination / 'netsurf/resources' / (name + '.css')).read_text()
        resources.append('static const char ns_resource_' + name + '[] =\n' +
                         '\n'.join(json.dumps(line, ensure_ascii=False) for line in css.splitlines(keepends=True)) + ';')
    write_changed(destination / 'netsurf/c16-resources.h', ('\n'.join(resources) + '\n').encode())
    # Run upstream's generators in the working tree. Fixed hash iteration is
    # essential: make-entities.pl otherwise generates a different trie each run.
    environment = dict(os.environ, PERL_HASH_SEED='0', PERL_PERTURB_KEYS='0')
    for library, generator in [('libparserutils', 'make-aliases.pl'), ('libhubbub', 'make-entities.pl')]:
        subprocess.run(['perl', 'build/' + generator], cwd=destination / library,
                       env=environment, check=True)
    hubbub = destination / 'libhubbub'
    result = subprocess.check_output(['gperf', 'src/treebuilder/element-type.gperf'], cwd=hubbub)
    # Same visibility adjustment as upstream Makefile (without relying on sed).
    result = result.replace(b'\nconst struct element_type_map', b'\nstatic const struct element_type_map')
    write_changed(hubbub / 'src/treebuilder/autogenerated-element-type.c', result)
    css = destination / 'libcss/src/parse/properties'
    generator = destination / 'gen-css-properties'
    generator_source = css / 'css_property_parser_gen.c'
    if not generator.exists() or generator.stat().st_mtime_ns < generator_source.stat().st_mtime_ns:
        subprocess.run(['cc', str(generator_source), '-o', str(generator)], check=True)
    for line in (css / 'properties.gen').read_text().splitlines():
        if not line or line.startswith('#'):
            continue
        name = line.split(':', 1)[0]
        generated = css / ('autogenerated_' + name + '.c')
        with tempfile.TemporaryDirectory(dir=destination) as temporary:
            raw = Path(temporary) / 'property.c'
            subprocess.run([str(generator), '-o', str(raw), line], check=True)
            write_changed(generated, adapt('libcss/src/parse/properties/' + generated.name,
                                          raw.read_text()).encode())
    write_changed(destination / 'adaptations.json', json.dumps(changed, indent=2, sort_keys=True).encode() + b'\n')
    return destination


if __name__ == '__main__':
    import argparse
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('output', type=Path)
    print(prepare(parser.parse_args().output))
