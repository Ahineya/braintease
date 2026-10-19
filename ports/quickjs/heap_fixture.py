"""Extract the actual production pool allocator, with its real headers."""
from storage_fixture import definition


def fixture(source):
    types = source[source.index('#define JS_MALLOC_ALIGN '):source.index('/* end JS Malloc */')]
    start = source.index('static const uint16_t js_malloc_block_sizes')
    allocator = source[start:source.index('static __maybe_unused void js_malloc_dump_arenas', start)]
    tags = '\n'.join(definition(source, name)[1] for name in
                     ('atom_get_free', 'atom_is_free', 'atom_set_free'))
    return '\n'.join(['/* Generated production headers/allocator, no engine runtime. */',
                      types, allocator, 'typedef struct JSString JSAtomStruct;', tags]) + '\n'
