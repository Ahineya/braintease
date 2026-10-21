/* Canonical types preserve structural distinctions, including equal sizes. */
#include "../../ripple/ripple.h"
#include <assert.h>
#include <string.h>

int main(void)
{
    static const char source[] = "module cache { target native "
                                 "@a = declaration struct A {i16*, [2 x i32]} "
                                 "@b = declaration struct B {i16*, [2 x i32]} "
                                 "@c = declaration struct {i32, i64} "
                                 "@d = declaration struct {i64, i32} "
                                 "@e = declaration [2 x i16] "
                                 "@f = declaration [1 x i32] "
                                 "@g = declaration i1* "
                                 "@h = declaration i8* "
                                 "@i = declaration i16* "
                                 "@j = declaration i16* "
                                 "@k = declaration struct {struct {i16*, [2 x i32]}*, [0 x i16]} "
                                 "@l = declaration struct {struct {i16*, [2 x i32]}*, [0 x i16]} "
                                 "@m = declaration [0 x i32] }";
    FILE *input = tmpfile();
    struct module *module;
    struct type *a, *k;
    filename = "type-cache";
    assert(input && fwrite(source, 1, strlen(source), input) == strlen(source));
    rewind(input);
    module = parse_target(input, filename, "native");
    a = symbol(module, "a")->type;
    k = symbol(module, "k")->type;
    assert(a == symbol(module, "b")->type);
    assert(k == symbol(module, "l")->type);
    assert(((struct type **)k->fields.data)[0]->base == a);
    assert(symbol(module, "c")->type != symbol(module, "d")->type);
    assert(symbol(module, "e")->type != symbol(module, "f")->type);
    assert(symbol(module, "g")->type != symbol(module, "h")->type);
    assert(symbol(module, "h")->type != symbol(module, "i")->type);
    assert(symbol(module, "i")->type == symbol(module, "j")->type);
    assert(((struct type **)k->fields.data)[1] != symbol(module, "m")->type);
    assert(fclose(input) == 0);
    puts("Structural type sharing and equal-size type distinctions passed");
    return 0;
}
