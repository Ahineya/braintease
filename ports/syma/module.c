#include "syma.h"
#include "builtins.h"
#include <ctype.h>
typedef struct Import Import;
typedef struct Module Module;
struct Import {
    Import *next;
    Module *module;
    Id name, alias, opens;
    unsigned open, meta;
};
struct Module {
    Module *next;
    Id name, ast, exports, defs, rules, rr, program;
    char *path;
    Import *imports;
    unsigned visiting, done;
};
static Module *modules;
static char libraryhint[1024];
void syma_library_hint(const char *executable) {
    const char *slash = strrchr(executable, 47);
    if (slash && (Count)(slash - executable) < 990) {
        memcpy(libraryhint, executable, (slash - executable) * sizeof(char));
        strcpy(libraryhint + (slash - executable), "/syma/library");
    }
}
static char *joinpath(const char *a, const char *b) {
    if (b[0] == '/') {
        char *absolute = syma_mem(((Count)strlen(b) + 1) * sizeof(char));
        strcpy(absolute, b);
        return absolute;
    }
    char *s = syma_mem((Count)(strlen(a) + strlen(b) + 2) * sizeof(char));
    strcpy(s, a);
    strcat(s, "/");
    strcat(s, b);
    return s;
}
static char *dirnameof(const char *path) {
    char *s = syma_mem(((Count)strlen(path) + 1) * sizeof(char)), *p;
    strcpy(s, path);
    p = strrchr(s, '/');
    if (p)
        *p = 0;
    else
        strcpy(s, ".");
    return s;
}
static int exists(const char *p) {
    FILE *f = fopen(p, "r");
    if (!f)
        return 0;
    fclose(f);
    return 1;
}
static Module *findmodule(Id name) {
    Module *m;
    for (m = modules; m; m = m->next)
        if (m->name == name)
            return m;
    return NULL;
}
static char *modulepath(Id name, const char *relative) {
    char *n = syma_utf8(name), *s, *paths, *end, *q;
    Count i;
    if (relative) {
        s = joinpath(relative, n);
        if (exists(s)) {
            free(n);
            return s;
        }
        free(s);
    }
    for (i = 0; n[i]; i++)
        n[i] = n[i] == '/' ? '-' : (char)tolower((unsigned char)n[i]);
    q = syma_mem(((Count)strlen(n) + 6) * sizeof(char));
    strcpy(q, n);
    strcat(q, ".syma");
    free(n);
    if (libraryhint[0] && !getenv("SYMA_PATH")) {
        s = joinpath(libraryhint, q);
        if (exists(s)) {
            free(q);
            return s;
        }
        free(s);
    }
    paths = getenv("SYMA_PATH");
    if (!paths)
        paths = "/usr/share/syma/library:ports/syma/library";
    while (*paths) {
        end = strchr(paths, ':');
        i = end ? (Count)(end - paths) : (Count)strlen(paths);
        n = syma_mem((i + 1) * sizeof(char));
        memcpy(n, paths, i * sizeof(char));
        s = joinpath(n, q);
        free(n);
        if (exists(s)) {
            free(q);
            return s;
        }
        free(s);
        if (!end)
            break;
        paths = end + 1;
    }
    free(q);
    return NULL;
}
static Module *readmodule(const char *path, Id expected, unsigned depth);
static Module *astmodule(Id ast, const char *path, Id expected, unsigned depth) {
    Module *m, *known;
    Import **tail;
    Id im, v;
    Count i;
    char *text, *base, *dep;
    if (depth > 64)
        syma_fail("module dependency limit exceeded");
    if (!syma_headis(ast, "Module") || !syma_argcnt(ast) ||
        syma_node(syma_arg(ast, 0))->kind != SYM)
        syma_fail("expected Module declaration");
    if (expected && syma_arg(ast, 0) != expected)
        syma_fail("imported module name mismatch");
    known = findmodule(syma_arg(ast, 0));
    if (known) {
        if (known->visiting)
            syma_fail("circular module dependency");
        return known;
    }
    m = syma_mem(sizeof(Module));
    m->name = syma_arg(ast, 0);
    m->ast = ast;
    m->path = syma_mem(((Count)strlen(path) + 1) * sizeof(char));
    strcpy(m->path, path);
    m->next = modules;
    modules = m;
    m->visiting = 1;
    m->exports = syma_section(ast, "Export");
    m->defs = syma_section(ast, "Defs");
    m->rules = syma_section(ast, "Rules");
    m->rr = syma_section(ast, "RuleRules");
    m->program = syma_section(ast, "Program");
    tail = &m->imports;
    base = dirnameof(path);
    for (i = 1; i < syma_argcnt(ast); i++) {
        im = syma_arg(ast, i);
        if (!syma_headis(im, "Import"))
            continue;
        Count j = 0;
        while (j < syma_argcnt(im)) {
            Import *in = syma_mem(sizeof(Import));
            *tail = in;
            tail = &in->next;
            in->name = syma_arg(im, j++);
            in->alias = in->name;
            if (j < syma_argcnt(im) && syma_is(syma_arg(im, j), "as")) {
                j++;
                in->alias = syma_arg(im, j++);
            }
            dep = NULL;
            if (j < syma_argcnt(im) && syma_is(syma_arg(im, j), "from")) {
                j++;
                v = syma_arg(im, j++);
                if (!v || syma_node(v)->kind != STR)
                    syma_fail("expected import path string");
                text = syma_utf8(v);
                dep = joinpath(base, text);
                free(text);
            }
            while (j < syma_argcnt(im)) {
                v = syma_arg(im, j);
                if (syma_is(v, "open"))
                    in->open = 1;
                else if (syma_is(v, "macro"))
                    in->meta = 1;
                else if (syma_headis(v, "Open"))
                    in->opens = v;
                else
                    break;
                j++;
            }
            if (!dep)
                dep = modulepath(in->name, base);
            if (!dep)
                syma_fail("cannot resolve module (set SYMA_PATH)");
            in->module = readmodule(dep, in->name, depth + 1);
            free(dep);
        }
    }
    free(base);
    m->visiting = 0;
    return m;
}
static Module *readmodule(const char *path, Id expected, unsigned depth) {
    char *text = syma_readfile(path);
    Id ast = syma_parse(text);
    free(text);
    return astmodule(ast, path, expected, depth);
}
static int contains(Id callnode, Id value) {
    Count i;
    for (i = 0; i < syma_argcnt(callnode); i++)
        if (syma_arg(callnode, i) == value)
            return 1;
    return 0;
}
static Id qualify_symbol(Module *m, Id x) {
    char *s = syma_utf8(x), *p, *alias, *name, *joined;
    Count i;
    Import *in;
    Id r = x;
    if (!s[0] || s[0] == ':') {
        free(s);
        return x;
    }
    p = strchr(s, '/');
    if (p) {
        for (in = m->imports; in; in = in->next) {
            alias = syma_utf8(in->alias);
            i = (Count)strlen(alias);
            if (!strncmp(s, alias, i) && s[i] == '/') {
                name = syma_utf8(in->name);
                joined = joinpath(name, s + i + 1);
                r = syma_sym(joined);
                free(name);
                free(joined);
                free(alias);
                free(s);
                return r;
            }
            free(alias);
        }
        *p = 0;
        Module *mod = findmodule(syma_sym(s));
        *p = '/';
        if (mod) {
            free(s);
            return x;
        }
    }
    for (i = 0; builtins[i]; i++)
        if (!strcmp(s, builtins[i])) {
            free(s);
            return x;
        }
    for (in = m->imports; in; in = in->next)
        if (contains(in->opens, x)) {
            name = syma_utf8(in->name);
            joined = joinpath(name, s);
            r = syma_sym(joined);
            free(name);
            free(joined);
            free(s);
            return r;
        }
    if (!contains(m->exports, x)) {
        for (in = m->imports; in; in = in->next)
            if (in->open && contains(in->module->exports, x)) {
                name = syma_utf8(in->name);
                joined = joinpath(name, s);
                r = syma_sym(joined);
                free(name);
                free(joined);
                free(s);
                return r;
            }
        for (in = m->imports; in; in = in->next)
            if (in->alias == x) {
                free(s);
                return x;
            }
        if (p) {
            free(s);
            return x;
        }
    }
    name = syma_utf8(m->name);
    joined = joinpath(name, s);
    r = syma_sym(joined);
    free(name);
    free(joined);
    free(s);
    return r;
}
static Id qualify(Module *m, Id x, unsigned depth) {
    Seq *a = NULL;
    Id h;
    Count i;
    if (!x)
        return 0;
    if (depth > 256)
        syma_fail("module term nesting limit exceeded");
    if (syma_node(x)->kind == SYM)
        return qualify_symbol(m, x);
    if (syma_node(x)->kind != CALL)
        return x;
    h = qualify(m, syma_node(x)->h, depth + 1);
    for (i = 0; i < syma_argcnt(x); i++)
        a = syma_cat(a, syma_one(qualify(m, syma_arg(x, i), depth + 1)));
    return syma_call(h, a);
}
static void linkmodule(Module *m, Seq **rs, Seq **rr, Seq **scopes, Module *global) {
    Import *in;
    Seq *scope = NULL;
    Count i;
    Id tag;
    char *name, *defname, *full;
    Id ds;
    if (m->done)
        return;
    for (in = m->imports; in; in = in->next)
        linkmodule(in->module, rs, rr, scopes, global);
    m->done = 1;
    name = syma_utf8(m->name);
    tag = syma_str(name);
    free(name);
    if (global)
        scope = syma_cat(scope, syma_one(syma_str("Core/Syntax/Global")));
    if (syma_argcnt(m->rr))
        scope = syma_cat(scope, syma_one(tag));
    for (in = m->imports; in; in = in->next)
        if (in->meta) {
            name = syma_utf8(in->name);
            scope = syma_cat(scope, syma_one(syma_str(name)));
            free(name);
        }
    *scopes = syma_cat(
        *scopes,
        syma_one(syma_named(
            "Module", syma_cat(syma_one(tag), syma_one(syma_named("RuleRulesFrom", scope))))));
    for (i = 0; i < syma_argcnt(m->rules); i++)
        *rs = syma_cat(
            *rs, syma_one(syma_named(
                     "TaggedRule",
                     syma_cat(syma_one(tag), syma_one(qualify(m, syma_arg(m->rules, i), 0))))));
    for (i = 0; i < syma_argcnt(m->rr); i++)
        *rr = syma_cat(*rr,
                       syma_one(syma_named(
                           "TaggedRuleRule",
                           syma_cat(syma_one(tag), syma_one(qualify(m, syma_arg(m->rr, i), 0))))));
    for (i = 0; i < syma_argcnt(m->defs); i++) {
        ds = syma_arg(m->defs, i);
        if (syma_node(ds)->kind != CALL || !syma_node(ds)->h || !syma_argcnt(ds))
            continue;
        name = syma_utf8(m->name);
        defname = syma_utf8(syma_node(ds)->h);
        full = joinpath(name, defname);
        Id lhs = syma_call(syma_sym(full), NULL);
        char *rn = joinpath(full, "DefCall");
        Id rule = syma_named("R", syma_cat(syma_cat(syma_cat(syma_one(syma_str(rn)), syma_one(lhs)),
                                                    syma_one(qualify(m, syma_arg(ds, 0), 0))),
                                           syma_one(syma_number(1000))));
        *rs = syma_cat(syma_one(syma_named("TaggedRule", syma_cat(syma_one(tag), syma_one(rule)))),
                       *rs);
        free(rn);
        free(name);
        free(defname);
        free(full);
    }
}
static Id compile_module(Id x, const char *path, Id *exports) {
    char *globalpath;
    Module *entry, *global = NULL;
    Seq *rs = NULL, *rr = NULL, *scopes = NULL, *a = NULL;
    syma_modules_clear();
    if (syma_headis(x, "Universe"))
        return x;
    if (!syma_headis(x, "Module"))
        return x;
    entry = astmodule(x, path, 0, 0);
    if (exports && entry->imports)
        *exports = entry->imports->module->exports;
    globalpath = modulepath(syma_sym("Core/Syntax/Global"), NULL);
    if (globalpath) {
        global = readmodule(globalpath, syma_sym("Core/Syntax/Global"), 0);
        free(globalpath);
        linkmodule(global, &rs, &rr, &scopes, global);
    }
    linkmodule(entry, &rs, &rr, &scopes, global);
    if (global)
        scopes = syma_cat(
            scopes, syma_one(syma_named(
                        "Module",
                        syma_cat(syma_one(syma_str("*")),
                                 syma_one(syma_named("RuleRulesFrom",
                                                     syma_one(syma_str("Core/Syntax/Global"))))))));
    a = entry->program ? syma_one(qualify(entry, entry->program, 0)) : NULL;
    a = syma_cat(a, syma_one(syma_named("Rules", rs)));
    a = syma_cat(a, syma_one(syma_named("RuleRules", rr)));
    a = syma_cat(a, syma_one(syma_named("MacroScopes", scopes)));
    x = syma_named("Universe", a);
    syma_modules_clear();
    return x;
}
Id syma_compile_module(Id x, const char *path) { return compile_module(x, path, NULL); }
Id syma_import_library(Id request) {
    Id exports = 0;
    Id ast = syma_named("Module", syma_cat(syma_one(syma_sym("$ReplImport")), syma_one(request)));
    Id universe = compile_module(ast, "./repl.syma", &exports);
    return syma_named(
        "Library",
        syma_cat(syma_one(universe), syma_one(exports ? exports : syma_named("Export", NULL))));
}
void syma_modules_clear(void) {
    Module *m, *next;
    Import *i, *in;
    for (m = modules; m; m = next) {
        next = m->next;
        for (i = m->imports; i; i = in) {
            in = i->next;
            free(i);
        }
        free(m->path);
        free(m);
    }
    modules = NULL;
}
static int allowed(Id scopes, Id source, Id target) {
    Count i, j;
    for (i = 0; i < syma_argcnt(scopes); i++) {
        Id scope = syma_arg(scopes, i), tag = syma_arg(scope, 0);
        if (tag != source && !syma_is(tag, "*"))
            continue;
        Id from = syma_arg(scope, 1);
        for (j = 0; j < syma_argcnt(from); j++)
            if (syma_arg(from, j) == target)
                return 1;
    }
    return 0;
}
Id syma_rule_rules(Id universe, Id retained, Rules *retained_rules) {
    Id rs = syma_section(universe, "Rules"), rr = syma_section(universe, "RuleRules"),
       scopes = syma_section(universe, "MacroScopes"), tag, actual, v, result;
    Seq *out = NULL;
    Count i, j, k;
    Engine meta;
    syma_gc(universe, retained, retained_rules, NULL, 0);
    Count allocation_base = syma_allocated_sequences();
    for (i = 0; i < syma_argcnt(rs); i++) {
        v = syma_arg(rs, i);
        tag = syma_headis(v, "TaggedRule") ? syma_arg(v, 0) : 0;
        actual = tag ? syma_arg(v, 1) : v;
        memset(&meta, 0, sizeof(meta));
        syma_rules_init(&meta.rules);
        meta.depth = 1;
        meta.maxsteps = 10000;
        meta.preserve = meta.meta = 1;
        meta.universe = universe;
        for (j = 0; j < syma_argcnt(rr); j++) {
            Id r = syma_arg(rr, j);
            if (syma_headis(r, "TaggedRuleRule") && allowed(scopes, tag, syma_arg(r, 0)))
                syma_rules_add(&meta.rules, syma_arg(r, 1));
        }
        if (meta.rules.n) {
            syma_rules_index(&meta.rules);
            result = syma_normalize(&meta, syma_named("Rules", syma_one(actual)));
            actual = syma_arg(result, 0);
        }
        syma_rules_free(&meta.rules);
        Seq *expanded = syma_headis(actual, "Splat") || syma_headis(actual, "$splice")
                            ? syma_node(actual)->a
                            : syma_one(actual);
        for (k = 0; k < syma_length(expanded); k++) {
            result = syma_at(expanded, k);
            out = syma_cat(out, syma_one(tag ? syma_named("TaggedRule",
                                                          syma_cat(syma_one(tag), syma_one(result)))
                                             : result));
        }
        if (syma_allocated_sequences() - allocation_base > 1500) {
            Id keeper = syma_named("$rules", out);
            out = syma_node(keeper)->a;
            syma_gc(keeper, universe, retained_rules, &retained, 1);
            allocation_base = syma_allocated_sequences();
        }
    }
    return rs ? syma_replace_section(universe, "Rules", syma_named("Rules", out)) : universe;
}

Id syma_load_module(const char *path) {
    char *text = syma_readfile(path);
    Id ast = syma_parse(text);
    free(text);
    return syma_compile_module(ast, path);
}
