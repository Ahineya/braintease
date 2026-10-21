/* Uses only the public header: checks independent contexts, rollback and roots. */
#include "session.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static unsigned checks, failures;
static void expect(SymaContext *c, const char *source, const char *expected) {
    SymaResult r;
    checks++;
    if (!syma_context_eval(c, source, &r) || strcmp(r.text, expected)) {
        printf("EMBED FAIL: %s\nexpected: %s\nactual: %s\n", source, expected,
               r.error[0] ? r.error : r.text);
        failures++;
    }
    syma_result_free(&r);
}
static void error(SymaContext *c, const char *source) {
    SymaResult r;
    checks++;
    if (syma_context_eval(c, source, &r) || !r.error[0]) {
        printf("EMBED FAIL: expected error: %s\n", source);
        failures++;
    }
    syma_result_free(&r);
}
int main(int ac, char **av) {
    SymaContext *a, *b;
    SymaResult held;
    unsigned i;
    (void)ac;
    syma_runtime_open(av[0]);
    a = syma_context_new(2000);
    b = syma_context_new(2000);
    expect(a, "{R double {Times x_} {Mul 2 x_}}", "{Defined double}");
    expect(b, "{R triple {Times x_} {Mul 3 x_}}", "{Defined triple}");
    expect(a, "{Define $value {Times 21}}", "42");
    expect(b, "{Define $value {Times 21}}", "63");
    if (!syma_context_eval(a, "{Saved $value \"Привіт\"}", &held))
        return 2;
    for (i = 0; i < 180; i++) {
        expect(a, "{Times 5}", "10");
        expect(b, "{Times 5}", "15");
    }
    error(a, "{Define $value {Broken");
    error(a, "{Import Does/Not/Exist}");
    error(a, "{Define plain 4}");
    expect(a, "{Add $value 1}", "43");
    expect(b, "{Add $value 1}", "64");
    syma_context_set_limit(a, 1);
    error(a, "{Define $value {Times 100}}");
    syma_context_set_limit(a, 2000);
    expect(a, "{Times 5}", "10");
    expect(a, "{Add $value 1}", "43");
    syma_context_set_limit(a, 0);
    expect(a, "{Times 5}", "10");
    checks++;
    if (strcmp(held.text, "{Saved 42 \"Привіт\"}"))
        failures++;
    syma_result_free(&held);
    syma_context_free(a);
    a = syma_context_new(2000);
    expect(a, "{R loop {Loop x_} {Loop {Add x_ 1}}}", "{Defined loop}");
    error(a, "{Define $bad {Loop 0}}");
    expect(a, "{Add 20 22}", "42");
    expect(b, "{Times 4}", "12");
    syma_context_free(a);
    a = syma_context_new(100000);
    expect(a, "{Module One {Export F} {Rules {R \"same\" {F x_} {Add x_ 1}}}}", "{Defined One}");
    expect(a, "{Module Two {Export SecondFn} {Rules {R \"same\" {SecondFn x_} {Add x_ 2}}}}",
           "{Defined Two}");
    expect(a, "{One/F 40}", "41");
    expect(a, "{Two/SecondFn 40}", "42");
    expect(a, "{Import Math/Small as M open}", "{Imported Math/Small}");
    expect(a, "{Define $p {Sum {Power x 3} {Product 2 x}}}",
           "{Math/Small/Sum {Math/Small/Power x 3} {Math/Small/Product 2 x}}");
    expect(a, "{Define $d {Differentiate $p x}}",
           "{Math/Small/Sum {Math/Small/Product 3 {Math/Small/Power x 2}} 2}");
    expect(a, "{Substitute $p x 3}", "33");
    expect(a, "{Substitute $d x 3}", "29");
    expect(a, "{M/Power 0 0}", "{Math/Small/Power 0 0}");
    expect(a, "{M/Power x -1}", "{Math/Small/Power x -1}");
    expect(a, "{Differentiate {Frob x} x}", "{Math/Small/UnevaluatedDerivative {Frob x} x}");
    expect(a, "{Substitute {Frob x {Box x}} x 7}", "{Frob 7 {Box 7}}");
    expect(a, "{Differentiate {Cos x} x}", "{Math/Small/Product -1 {Math/Small/Sin x}}");
    expect(a, "{Differentiate {Power x 1.5} x}",
           "{Math/Small/UnevaluatedDerivative {Math/Small/Power x 1.5} x}");
    expect(a, "{Import OOP/Objects as O}", "{Imported OOP/Objects}");
    expect(a,
           "{O/Run {Program :class Base {sound() => \"base\"} :class Child :extends Base {sound() "
           "=> \"child\"} :var obj := :new Child() :print(obj.sound()) :print(obj is Base) "
           ":print(obj is Other)}}",
           "{Output \"child\" true false}");
    expect(a, "{Substitute $d x 3}", "29");
    expect(a, "{Import Core/List as L}", "{Imported Core/List}");
    expect(a, "{L/Head {List 11 12}}", "11");
    syma_context_free(a);
    syma_context_free(b);
    syma_runtime_close();
    printf("EMBED checks=%u failures=%u\n", checks, failures);
    return failures ? 1 : 0;
}
