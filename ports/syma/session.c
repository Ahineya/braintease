#include "syma.h"
#include "session.h"
#include "builtins.h"

struct SymaContext {
    SymaContext *next;
    Engine engine;
    Rules building;
    Id transaction, last, oldlast, pending;
    char *scratch;
};
static SymaContext *contexts;

void syma_mark_contexts(void (*mark)(Id)) {
    SymaContext *c;
    for (c = contexts; c; c = c->next) {
        mark(c->engine.universe);
        mark(c->engine.keep);
        mark(c->transaction);
        mark(c->last);
        mark(c->oldlast);
        mark(c->pending);
        Count i;
        for (i = 0; i < c->engine.rules.n; i++) {
            Rule *r = &c->engine.rules.v[i];
            mark(r->ast);
            mark(r->name);
            mark(r->lhs);
            mark(r->rhs);
            mark(r->guard);
            mark(r->scope);
            mark(r->with);
            mark(r->let);
            mark(r->key);
        }
    }
}
void syma_runtime_open(const char *executable) {
    syma_store_init();
    if (executable)
        syma_library_hint(executable);
}
SymaContext *syma_context_new(uint32_t maxsteps) {
    SymaContext *c = syma_mem(sizeof(*c));
    c->engine.maxsteps = maxsteps ? maxsteps : 10000;
    c->engine.universe = syma_named("Universe", syma_one(syma_named("Rules", NULL)));
    c->next = contexts;
    contexts = c;
    return c;
}
void syma_context_set_limit(SymaContext *c, uint32_t maxsteps) {
    c->engine.maxsteps = maxsteps ? maxsteps : 10000;
}
void syma_context_free(SymaContext *c) {
    SymaContext **p = &contexts;
    while (*p && *p != c)
        p = &(*p)->next;
    if (!*p)
        return;
    *p = c->next;
    syma_rules_free(&c->engine.rules);
    syma_rules_free(&c->building);
    free(c->scratch);
    free(c);
}
void syma_runtime_close(void) {
    while (contexts)
        syma_context_free(contexts);
    syma_effects_cleanup();
    syma_modules_clear();
    syma_store_free();
}
void syma_result_free(SymaResult *r) {
    free(r->text);
    memset(r, 0, sizeof(*r));
}
static void install(SymaContext *c, Id universe) {
    syma_rules_free(&c->building);
    syma_rules_extract(&c->building, universe);
    syma_rules_free(&c->engine.rules);
    c->engine.rules = c->building;
    memset(&c->building, 0, sizeof(c->building));
    c->engine.universe = universe;
}
static void merge(SymaContext *c, Id incoming) {
    const char *names[] = {"Rules", "RuleRules", "MacroScopes", "Bindings", "Imports", NULL};
    Id u = c->engine.universe;
    Count i, j, k;
    for (i = 0; names[i]; i++) {
        Id old = syma_section(u, names[i]), add = syma_section(incoming, names[i]);
        Seq *a = old ? syma_node(old)->a : NULL;
        for (j = 0; j < syma_argcnt(add); j++) {
            Id item = syma_arg(add, j);
            for (k = 0; k < syma_length(a); k++) {
                Id previous = syma_at(a, k);
                if (previous == item)
                    break;
            }
            if (k == syma_length(a))
                a = syma_cat(a, syma_one(item));
        }
        u = syma_replace_section(u, names[i], syma_named(names[i], a));
    }
    c->pending = u;
    install(c, syma_rule_rules(u, c->engine.universe, &c->engine.rules));
}
static Id resolve_export(SymaContext *c, Id module, Id names, const char *tail) {
    char *prefix = syma_utf8(module);
    Buffer b = {0};
    syma_buftext(&b, prefix);
    syma_bufput(&b, '/');
    syma_buftext(&b, tail);
    free(prefix);
    Id full = syma_sym(b.p), local = syma_sym(tail);
    free(b.p);
    Count i, j;
    for (i = 0; i < c->engine.rules.n; i++)
        if (c->engine.rules.v[i].key == full)
            return full;
    for (i = 0; builtins[i]; i++)
        if (!strcmp(tail, builtins[i]))
            for (j = 0; j < syma_argcnt(names); j++)
                if (syma_arg(names, j) == local)
                    return local;
    return full;
}
static Id qualify(SymaContext *c, Id x, unsigned depth) {
    if (!x)
        return x;
    if (depth > 256)
        syma_fail("cell nesting limit exceeded");
    Node *p = syma_node(x);
    Count i, j;
    if (p->kind == SYM) {
        if (syma_is(x, "$it") && c->last)
            return c->last;
        Id bindings = syma_section(c->engine.universe, "Bindings");
        for (i = syma_argcnt(bindings); i > 0; i--) {
            Id pair = syma_arg(bindings, i - 1);
            if (x == syma_arg(pair, 0))
                return syma_arg(pair, 1);
        }
        char *text = syma_utf8(x);
        Id uses = syma_section(c->engine.universe, "Imports"), out = x;
        for (i = syma_argcnt(uses); i > 0; i--) {
            Id use = syma_arg(uses, i - 1);
            char *prefix = syma_utf8(syma_arg(use, 1));
            Count n = (Count)strlen(prefix);
            unsigned match = !strncmp(text, prefix, n) && text[n] == '/';
            free(prefix);
            if (match) {
                out = resolve_export(c, syma_arg(use, 0), syma_arg(use, 2), text + n + 1);
                break;
            }
            if (syma_is(syma_arg(use, 3), "True")) {
                Id exports = syma_arg(use, 2);
                for (j = 0; j < syma_argcnt(exports); j++)
                    if (syma_arg(exports, j) == x) {
                        out = resolve_export(c, syma_arg(use, 0), exports, text);
                        break;
                    }
                if (out != x)
                    break;
            }
        }
        free(text);
        return out;
    }
    if (p->kind != CALL || syma_headis(x, "Var") || syma_headis(x, "VarRest") ||
        syma_headis(x, "/"))
        return x;
    Id h = qualify(c, p->h, depth + 1);
    Seq *a = NULL;
    for (i = 0; i < syma_argcnt(x); i++)
        a = syma_cat(a, syma_one(qualify(c, syma_arg(x, i), depth + 1)));
    return syma_call(h, a);
}
static Id import(SymaContext *c, Id x) {
    Id name = syma_arg(x, 0), alias = name;
    unsigned open = 0;
    Count i;
    if (!name || syma_node(name)->kind != SYM)
        syma_fail("Import needs a module name");
    for (i = 1; i < syma_argcnt(x); i++) {
        Id option = syma_arg(x, i);
        if (syma_is(option, "as") && i + 1 < syma_argcnt(x))
            alias = syma_arg(x, ++i);
        else if (syma_is(option, "open"))
            open = 1;
        else if (syma_is(option, "from") && i + 1 < syma_argcnt(x))
            i++;
        else
            syma_fail("Import supports as, from and open");
    }
    Id library = syma_import_library(x);
    Id use = syma_named("Use", syma_cat(syma_cat(syma_cat(syma_one(name), syma_one(alias)),
                                                 syma_one(syma_arg(library, 1))),
                                        syma_one(syma_sym(open ? "True" : "False"))));
    c->pending = syma_named("ImportKeep", syma_cat(syma_one(library), syma_one(use)));
    merge(c, syma_replace_section(syma_arg(library, 0), "Imports",
                                  syma_named("Imports", syma_one(use))));
    return syma_named("Imported", syma_one(name));
}
int syma_context_eval(SymaContext *c, const char *source, SymaResult *result) {
    jmp_buf boundary;
    jmp_buf *previous = syma_error_target;
    Id value;
    memset(result, 0, sizeof(*result));
    c->transaction = c->engine.universe;
    c->oldlast = c->last;
    c->engine.rewrites = c->engine.primitive_steps = 0;
    syma_error_target = &boundary;
    if (setjmp(boundary)) {
        strcpy(result->error, syma_message);
        syma_match_cleanup();
        syma_visit_cleanup();
        syma_modules_clear();
        install(c, c->transaction);
        c->last = c->oldlast;
        c->engine.depth = c->engine.excluded_count = c->engine.excluded = 0;
        c->engine.keep = 0;
        c->pending = c->transaction = c->oldlast = 0;
        free(c->scratch);
        c->scratch = NULL;
        syma_error_target = previous;
        syma_gc(0, 0, NULL, NULL, 0);
        return 0;
    }
    if (strlen(source) > 1048576UL)
        syma_fail("cell exceeds 1 MiB");
    Id x = syma_parse(source);
    c->pending = c->engine.keep = x;
    if (syma_headis(x, "Import"))
        value = import(c, x);
    else if (syma_headis(x, "Module")) {
        merge(c, syma_compile_module(x, "./worksheet.syma"));
        value = syma_named("Defined", syma_one(syma_arg(x, 0)));
    } else if (syma_headis(x, "R")) {
        merge(c, syma_named("Universe", syma_one(syma_named("Rules", syma_one(x)))));
        value = syma_named("Defined", syma_one(syma_arg(x, 0)));
    } else if (syma_headis(x, "Define")) {
        Id name = syma_arg(x, 0);
        if (syma_argcnt(x) != 2 || !name || syma_node(name)->kind != SYM)
            syma_fail("Define expects a name and an expression");
        c->scratch = syma_utf8(name);
        if (c->scratch[0] != '$' || !strcmp(c->scratch, "$it"))
            syma_fail("Define names start with $; $it is reserved");
        free(c->scratch);
        c->scratch = NULL;
        value = syma_normalize(&c->engine, qualify(c, syma_arg(x, 1), 0));
        c->last = value;
        Id old = syma_section(c->engine.universe, "Bindings");
        Seq *a = NULL;
        Count i;
        for (i = 0; i < syma_argcnt(old); i++)
            if (syma_arg(syma_arg(old, i), 0) != name)
                a = syma_cat(a, syma_one(syma_arg(old, i)));
        a = syma_cat(a, syma_one(syma_named("Binding", syma_cat(syma_one(name), syma_one(value)))));
        c->engine.universe =
            syma_replace_section(c->engine.universe, "Bindings", syma_named("Bindings", a));
    } else {
        value = syma_normalize(&c->engine, qualify(c, x, 0));
        c->last = value;
    }
    result->text = syma_show(value, 0);
    result->rewrites = c->engine.rewrites;
    result->primitives = c->engine.primitive_steps;
    c->engine.keep = 0;
    c->transaction = c->oldlast = c->pending = 0;
    syma_error_target = previous;
    syma_gc(0, 0, NULL, NULL, 0);
    return 1;
}
