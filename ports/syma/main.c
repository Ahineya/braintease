#include "syma.h"
#include <unistd.h>
static Engine engine;
static int cases(const char *path) {
    FILE *f = fopen(path, "r");
    Buffer b = {0};
    Id request, result;
    Count checks = 0, failures = 0;
    volatile unsigned expected_error;
    int c, err;
    char *actual, *expected;
    if (!f)
        syma_fail("cannot open test corpus");
    for (;;) {
        b.n = 0;
        while ((c = fgetc(f)) != EOF && c != '\n')
            syma_bufput(&b, (unsigned)c);
        if (!b.n) {
            if (c == EOF)
                break;
            continue;
        }
        printf("SYMA case=%lu\n", (unsigned long)checks);
        fflush(stdout);
        syma_store_init();
        memset(&engine, 0, sizeof(engine));
        expected_error = 0;
        err = setjmp(syma_error);
        if (!err) {
            request = syma_parse(b.p);
            engine.keep = request;
            engine.universe = syma_arg(request, 8);
            expected_error = syma_is(syma_arg(request, 5), "Error");
            syma_rules_init(&engine.rules);
            syma_rules_extract(&engine.rules, syma_arg(request, 0));
            engine.maxsteps = (Count)syma_node(syma_arg(request, 2))->num;
            engine.skipprims = syma_is(syma_arg(request, 3), "True");
            engine.preserve = syma_is(syma_arg(request, 4), "True");
            engine.meta = syma_is(syma_arg(request, 7), "True");
            result = syma_normalize(&engine, syma_arg(request, 1));
            if (expected_error || result != syma_arg(request, 6)) {
                actual = syma_show(result, 0);
                expected = syma_show(syma_arg(request, 6), 0);
                printf("FAIL %lu\nactual: %s\nexpected: %s\n", (unsigned long)checks, actual,
                       expected);
                free(actual);
                free(expected);
                failures++;
            }
        } else if (!expected_error) {
            printf("FAIL %lu: %s\n", (unsigned long)checks, syma_message);
            failures++;
        }
        checks++;
        fflush(stdout);
        syma_rules_free(&engine.rules);
        syma_store_free();
    }
    fclose(f);
    free(b.p);
    printf("SYMA checks=%lu failures=%lu\n", (unsigned long)checks, (unsigned long)failures);
    return failures ? 1 : 0;
}
static void init_engine(void) {
    memset(&engine, 0, sizeof(engine));
    syma_rules_init(&engine.rules);
    engine.maxsteps = 10000;
}
static void output(Id x) {
    char *text = syma_show(x, 0);
    puts(text);
    free(text);
}
int syma_main(int ac, char **av) {
    Id x;
    char *source;
    unsigned exited = 0;
    Count option;
    Seq *arguments = NULL;
    syma_store_init();
    init_engine();
    syma_library_hint(av[0]);
    if (setjmp(syma_error)) {
        syma_effects_cleanup();
        fprintf(stderr, "syma: %s\n", syma_message);
        return 1;
    }
    if (ac > 1 && !strcmp(av[1], "worksheet"))
        return syma_worksheet(ac, av);
    for (option = 3; option < (Count)ac; option++) {
        if (!strcmp(av[option], "--trace"))
            engine.trace = 1;
        else if (!strcmp(av[option], "--maxsteps") && option + 1 < (Count)ac) {
            engine.maxsteps = (Count)strtoul(av[++option], NULL, 10);
            if (!engine.maxsteps)
                syma_fail("maxsteps must be positive");
        } else if (!strcmp(av[option], "--arg") && option + 1 < (Count)ac) {
            char *spec = av[++option], *equal = strchr(spec, 61);
            if (!equal)
                syma_fail("--arg expects key=value");
            Id key = syma_utf8atom(STR, spec, (Count)(equal - spec));
            arguments = syma_cat(
                arguments,
                syma_one(syma_named("KV", syma_cat(syma_one(key), syma_one(syma_str(equal + 1))))));
        } else
            syma_fail("unknown command option");
    }
    if (ac > 2 && !strcmp(av[1], "--check"))
        return cases(av[2]);
    if (ac == 1 || !strcmp(av[1], "repl"))
        return syma_repl(&engine);
    if (ac > 2 && !strcmp(av[1], "eval")) {
        x = syma_parse(av[2]);
        output(syma_normalize(&engine, x));
        return 0;
    }
    if (ac > 2 &&
        (!strcmp(av[1], "compile") || !strcmp(av[1], "normal") || !strcmp(av[1], "run"))) {
        engine.universe = syma_load_module(av[2]);
        if (!strcmp(av[1], "compile")) {
            char *json = syma_show(engine.universe, 1);
            puts(json);
            free(json);
            return 0;
        }
        Id initial = syma_section(engine.universe, "Program");
        if (!initial && syma_headis(engine.universe, "Universe"))
            syma_fail("entry module needs a Program section");
        if (arguments && initial) {
            Id old = syma_section(initial, "Args");
            initial = syma_replace_section(
                initial, "Args",
                syma_named("Args", syma_cat(old ? syma_node(old)->a : NULL, arguments)));
            engine.universe = syma_replace_section(engine.universe, "Program", initial);
        }
        if (initial && !syma_section(initial, "Effects")) {
            initial = syma_replace_section(
                initial, "Effects",
                syma_named("Effects", syma_cat(syma_one(syma_named("Pending", NULL)),
                                               syma_one(syma_named("Inbox", NULL)))));
            engine.universe = syma_replace_section(engine.universe, "Program", initial);
        }
        engine.universe = syma_rule_rules(engine.universe, 0, NULL);
        syma_rules_extract(&engine.rules, engine.universe);
        x = syma_section(engine.universe, "Program");
        if (!x)
            x = engine.universe;
        if (!strcmp(av[1], "normal")) {
            char *json = syma_show(syma_normalize(&engine, x), 1);
            puts(json);
            free(json);
            return 0;
        }
        do {
            x = syma_normalize(&engine, x);
            x = syma_run_effects(&engine, x, &exited);
        } while (!exited);
        output(x);
        return syma_effect_status();
    }
    source = "Usage: syma run FILE | syma repl | syma eval EXPR";
    puts(source);
    return 2;
}
int main(int ac, char **av) { return syma_main(ac, av); }
