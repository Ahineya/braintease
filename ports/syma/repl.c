#include "syma.h"
#include "builtins.h"
#include <ctype.h>
#include <unistd.h>
#include <sys/stat.h>

/* Session data is explicit GC roots. Undo stores shared immutable Universes. */
typedef struct Scratch Scratch;
struct Scratch {
    Scratch *next;
    void *data;
};
static struct {
    Engine *vm;
    Id undo[64], transaction, pending, last;
    Count undos, lines, history_count;
    char *history[200], *reload;
    Scratch *scratch;
    Rules building;
    Buffer input;
    unsigned mode, failures, automatic_trace, trace_style, interactive;
} session;
#define engine (*session.vm)

void syma_mark_repl(void (*mark)(Id)) {
    Count i;
    if (!session.vm)
        return;
    mark(session.transaction);
    mark(session.pending);
    mark(session.last);
    for (i = 0; i < session.undos; i++)
        mark(session.undo[i]);
}
static void *temporary(Count size) {
    Scratch *s = syma_mem(sizeof(*s));
    s->next = session.scratch;
    session.scratch = s;
    s->data = syma_mem(size);
    return s->data;
}
static char *copy(const char *text) {
    char *out = temporary(((Count)strlen(text) + 1) * sizeof(char));
    strcpy(out, text);
    return out;
}
static char *own(char *text) {
    Scratch *s = syma_mem(sizeof(*s));
    s->next = session.scratch;
    session.scratch = s;
    s->data = text;
    return text;
}
static void clear_temporary(void) {
    Scratch *s, *next;
    for (s = session.scratch; s; s = next) {
        next = s->next;
        free(s->data);
        free(s);
    }
    session.scratch = NULL;
    syma_rules_free(&session.building);
    free(engine.trace_counts);
    engine.trace_counts = NULL;
    session.pending = 0;
}
static char *trim(char *text) {
    Count n;
    while (isspace((unsigned char)*text))
        text++;
    n = (Count)strlen(text);
    while (n && isspace((unsigned char)text[n - 1]))
        text[--n] = 0;
    return text;
}
static char *word(char **input) {
    char *p = *input, *out, quote = 0;
    Count n = 0;
    while (isspace((unsigned char)*p))
        p++;
    out = temporary(((Count)strlen(p) + 1) * sizeof(char));
    if (*p == '"' || *p == '\'')
        quote = *p++;
    while (*p && (quote ? *p != quote : !isspace((unsigned char)*p))) {
        if (*p == '\\' && quote != '\'' && p[1])
            p++;
        out[n++] = *p++;
    }
    if (quote) {
        if (*p != quote)
            syma_fail("unterminated quoted argument");
        p++;
    }
    while (isspace((unsigned char)*p))
        p++;
    *input = p;
    return out;
}
static int suffix(const char *text, const char *tail) {
    Count a = (Count)strlen(text), b = (Count)strlen(tail);
    return a >= b && !strcmp(text + a - b, tail);
}
static int same_name(Id a, Id b) {
    if (a == b)
        return 1;
    if (!a || !b)
        return 0;
    Node *x = syma_node(a), *y = syma_node(b);
    return (x->kind == SYM || x->kind == STR) && (y->kind == SYM || y->kind == STR) &&
           x->len == y->len && !memcmp(x->s, y->s, x->len * sizeof(Unit));
}
static Id empty_universe(void) {
    return syma_named(
        "Universe",
        syma_cat(syma_one(syma_named(
                     "Program", syma_one(syma_named(
                                    "Effects", syma_cat(syma_one(syma_named("Pending", NULL)),
                                                        syma_one(syma_named("Inbox", NULL))))))),
                 syma_one(syma_named("Rules", NULL))));
}
static void install(Id universe) {
    if (!syma_headis(universe, "Universe"))
        syma_fail("expected Universe");
    syma_rules_extract(&session.building, universe);
    syma_rules_free(&engine.rules);
    engine.rules = session.building;
    syma_rules_init(&session.building);
    engine.universe = universe;
}
static void checkpoint(void) {
    if (session.undos == 64) {
        memmove(session.undo, session.undo + 1, 63 * sizeof(Id));
        session.undos--;
    }
    session.undo[session.undos++] = session.transaction;
}
static Id actual_rule(Id rule) {
    return syma_headis(rule, "TaggedRule") ? syma_arg(rule, 1) : rule;
}
static Id rule_name(Id rule) { return syma_arg(actual_rule(rule), 0); }
static Count find_rule(const char *name) {
    Id id = syma_str(name);
    Count i;
    for (i = 0; i < engine.rules.n; i++)
        if (same_name(engine.rules.v[i].name, id))
            return i;
    syma_fail("rule not found");
    return 0;
}
static void compact(Buffer *b, Id x) {
    char *s = syma_show(x, 0);
    syma_buftext(b, s);
    free(s);
}
static void pretty(Buffer *b, Id x, unsigned depth) {
    Count i;
    unsigned complex = 0, j;
    if (depth > 256)
        syma_fail("display nesting limit exceeded");
    if (!x || syma_node(x)->kind != CALL) {
        compact(b, x);
        return;
    }
    for (i = 0; i < syma_argcnt(x); i++)
        if (syma_node(syma_arg(x, i))->kind == CALL)
            complex++;
    if (!complex || depth > 8) {
        compact(b, x);
        return;
    }
    syma_bufput(b, '{');
    if (syma_node(x)->h)
        compact(b, syma_node(x)->h);
    for (i = 0; i < syma_argcnt(x); i++) {
        syma_bufput(b, '\n');
        for (j = 0; j < (depth + 1) * 2; j++)
            syma_bufput(b, ' ');
        pretty(b, syma_arg(x, i), depth + 1);
    }
    syma_bufput(b, '}');
}
static void output(Id x) {
    Buffer b = {0};
    pretty(&b, x, 0);
    puts(b.p ? b.p : "{}");
    free(b.p);
}
static void section(const char *name) {
    Id x = syma_section(engine.universe, name);
    if (x)
        output(x);
    else
        printf("No %s section defined in the universe\n", name);
}
static Id imported_symbol(Id module, Id names, const char *tail) {
    char *prefix = own(syma_utf8(module));
    char *full = temporary(((Count)strlen(prefix) + (Count)strlen(tail) + 2) * sizeof(char));
    strcpy(full, prefix);
    strcat(full, "/");
    strcat(full, tail);
    Id qualified = syma_sym(full), local = syma_sym(tail);
    Count i, j;
    for (i = 0; i < engine.rules.n; i++)
        if (engine.rules.v[i].key == qualified)
            return qualified;
    /* The module compiler deliberately retains built-in names such as Head.
     * Resolve an exported built-in to that actual binding for REPL aliases. */
    for (i = 0; builtins[i]; i++)
        if (!strcmp(tail, builtins[i])) {
            for (j = 0; j < syma_argcnt(names); j++)
                if (syma_arg(names, j) == local)
                    return local;
        }
    return qualified;
}
static Id qualify(Id x, unsigned depth) {
    Count i, j;
    Seq *a = NULL;
    Node *p = syma_node(x);
    if (!p)
        return x;
    if (depth > 256)
        syma_fail("import qualification nesting limit exceeded");
    if (p->kind == SYM) {
        if (syma_is(x, "$it") && session.last)
            return session.last;
        Id uses = syma_section(engine.universe, "ReplImports");
        char *text = own(syma_utf8(x));
        for (i = syma_argcnt(uses); i > 0; i--) {
            Id use = syma_arg(uses, i - 1), name = syma_arg(use, 0), alias = syma_arg(use, 1);
            char *prefix = own(syma_utf8(alias));
            Count n = (Count)strlen(prefix);
            if (!strncmp(text, prefix, n) && text[n] == '/') {
                return imported_symbol(name, syma_arg(use, 2), text + n + 1);
            }
        }
        for (i = 0; builtins[i]; i++)
            if (!strcmp(text, builtins[i]))
                return x;
        for (i = syma_argcnt(uses); i > 0; i--) {
            Id use = syma_arg(uses, i - 1), names = syma_arg(use, 2);
            if (!syma_is(syma_arg(use, 3), "True"))
                continue;
            for (j = 0; j < syma_argcnt(names); j++)
                if (syma_arg(names, j) == x) {
                    return imported_symbol(syma_arg(use, 0), names, text);
                }
        }
        return x;
    }
    if (p->kind != CALL || syma_headis(x, "Var") || syma_headis(x, "VarRest") ||
        syma_headis(x, "/"))
        return x;
    Id head = qualify(p->h, depth + 1);
    for (i = 0; i < syma_argcnt(x); i++)
        a = syma_cat(a, syma_one(qualify(syma_arg(x, i), depth + 1)));
    return syma_call(head, a);
}
static Id expression(const char *source) { return qualify(syma_parse(source), 0); }
static void merge_universe(Id incoming) {
    const char *sections[] = {"Rules",      "RuleRules",   "MacroScopes",
                              "ReplMacros", "ReplImports", NULL};
    Count i, j, k;
    Id universe = engine.universe;
    for (i = 0; sections[i]; i++) {
        Id old = syma_section(universe, sections[i]), add = syma_section(incoming, sections[i]);
        Seq *a = old ? syma_node(old)->a : NULL;
        for (j = 0; j < syma_argcnt(add); j++) {
            Id item = syma_arg(add, j);
            for (k = 0; k < syma_length(a); k++) {
                Id previous = syma_at(a, k);
                if (previous == item || (i == 0 && same_name(rule_name(previous), rule_name(item))))
                    break;
            }
            if (k == syma_length(a))
                a = syma_cat(a, syma_one(item));
        }
        universe = syma_replace_section(universe, sections[i], syma_named(sections[i], a));
    }
    install(syma_rule_rules(universe, engine.universe, &engine.rules));
}
static void add_rule(Id ast, unsigned editing) {
    Count i;
    Id imports = syma_section(engine.universe, "ReplMacros");
    Seq *args = syma_one(syma_arg(ast, 0));
    for (i = 1; i < syma_argcnt(ast); i++)
        args = syma_cat(args, syma_one(qualify(syma_arg(ast, i), 0)));
    ast = syma_named("R", args);
    Id incoming = syma_named("Rules", syma_one(ast));
    if (syma_argcnt(imports)) {
        Id tagged = syma_named("TaggedRule", syma_cat(syma_one(syma_str("$repl")), syma_one(ast)));
        Id u =
            syma_replace_section(engine.universe, "Rules", syma_named("Rules", syma_one(tagged)));
        Id scopes = syma_section(u, "MacroScopes");
        Id scope = syma_named(
            "Module", syma_cat(syma_one(syma_str("$repl")),
                               syma_one(syma_named("RuleRulesFrom", syma_node(imports)->a))));
        u = syma_replace_section(
            u, "MacroScopes",
            syma_named("MacroScopes",
                       syma_cat(scopes ? syma_node(scopes)->a : NULL, syma_one(scope))));
        u = syma_rule_rules(u, engine.universe, &engine.rules);
        incoming = syma_section(u, "Rules");
    }
    Id rules = syma_section(engine.universe, "Rules");
    Seq *all = NULL;
    for (i = 0; i < syma_argcnt(rules); i++) {
        Id r = syma_arg(rules, i);
        if (!editing || !same_name(rule_name(r), syma_arg(ast, 0)))
            all = syma_cat(all, syma_one(r));
    }
    all = syma_cat(all, incoming ? syma_node(incoming)->a : NULL);
    install(syma_replace_section(engine.universe, "Rules", syma_named("Rules", all)));
}
static Id inline_rule(char *source) {
    char *name = word(&source);
    if (!*name || !*source)
        syma_fail("usage: :rule NAME PATTERN -> REPLACEMENT");
    char *text = temporary(((Count)strlen(source) + 8) * sizeof(char));
    strcpy(text, "$rule ");
    strcat(text, source);
    Id ast = syma_inline_rule(text);
    return syma_named("R", syma_cat(syma_one(syma_str(name)),
                                    syma_slice(syma_node(ast)->a, 1, syma_argcnt(ast) - 1)));
}
static void drop_rule(char *name) {
    (void)find_rule(name);
    Id old = syma_section(engine.universe, "Rules");
    Seq *a = NULL;
    Count i;
    for (i = 0; i < syma_argcnt(old); i++)
        if (!same_name(rule_name(syma_arg(old, i)), syma_str(name)))
            a = syma_cat(a, syma_one(syma_arg(old, i)));
    install(syma_replace_section(engine.universe, "Rules", syma_named("Rules", a)));
}
static Id enrich(Id universe) {
    Id program = syma_section(universe, "Program");
    if (program && !syma_section(program, "Effects")) {
        program = syma_replace_section(
            program, "Effects",
            syma_named("Effects", syma_cat(syma_one(syma_named("Pending", NULL)),
                                           syma_one(syma_named("Inbox", NULL)))));
        universe = syma_replace_section(universe, "Program", program);
    }
    return universe;
}
static Id normalize(Id x, unsigned trace) {
    engine.trace = trace;
    engine.rewrites = engine.primitive_steps = 0;
    if (trace & 8)
        engine.trace_counts = syma_mem(engine.rules.n * sizeof(Count));
    double start = syma_now();
    x = syma_normalize(&engine, x);
    if (trace & 8) {
        Count i;
        printf("Trace statistics: %lu rewrites, %lu primitive-fold steps, %.3f s\n",
               (unsigned long)engine.rewrites, (unsigned long)engine.primitive_steps,
               (syma_now() - start) / 1000);
        for (i = 0; i < engine.rules.n; i++)
            if (engine.trace_counts[i])
                printf("  %s: %lu\n", own(syma_utf8(engine.rules.v[i].name)),
                       (unsigned long)engine.trace_counts[i]);
    }
    free(engine.trace_counts);
    engine.trace_counts = NULL;
    engine.trace = 0;
    session.last = x;
    return x;
}
static void run_program(void) {
    Id program = syma_section(engine.universe, "Program");
    unsigned done = 0;
    if (!program)
        syma_fail("no Program section to normalize");
    do {
        program = normalize(program, session.automatic_trace ? session.trace_style : 0);
        engine.universe = syma_replace_section(engine.universe, "Program", program);
        program = syma_run_effects(&engine, program, &done);
        engine.universe = syma_replace_section(engine.universe, "Program", program);
    } while (!done);
}
static void save_file(const char *path, Id value) {
    char *text = own(syma_show(value, suffix(path, ".json")));
    FILE *file = fopen(path, "w");
    if (!file)
        syma_fail("cannot open output file");
    int ok = fputs(text, file) >= 0;
    if (fclose(file))
        ok = 0;
    if (!ok)
        syma_fail("cannot write output file");
}
static void load_file(char *args, unsigned bundle, unsigned remember) {
    char *original = copy(args), *path = word(&args);
    if (!*path)
        syma_fail("usage: :load FILE / :bundle FILE [:args --KEY VALUE]");
    char *text = own(syma_readfile(path));
    Id x = suffix(path, ".json") ? syma_jsonparse(text) : syma_parse(text);
    if (syma_headis(x, "Module"))
        x = syma_compile_module(x, path);
    x = enrich(x);
    session.pending = x;
    if (*args) {
        char *marker = word(&args);
        if (strcmp(marker, ":args"))
            syma_fail("expected :args after filename");
        Seq *values = NULL;
        Count positional = 0;
        while (*args) {
            char *key = word(&args), *value = NULL;
            Id key_term, value_term;
            if (key[0] == '-' && key[1]) {
                unsigned short_flag = key[1] != '-';
                key += short_flag ? 1 : 2;
                char *equal = strchr(key, '=');
                if (equal) {
                    *equal = 0;
                    value = equal + 1;
                } else if (*args) {
                    char *lookahead = args, *candidate = word(&lookahead);
                    if (short_flag ? candidate[0] != '-' : strncmp(candidate, "--", 2) != 0) {
                        value = candidate;
                        args = lookahead;
                    }
                }
                key_term = syma_str(key);
                value_term = value ? syma_str(value) : syma_sym("Empty");
            } else {
                key_term = syma_number(positional++);
                value_term = syma_str(key);
            }
            values = syma_cat(values, syma_one(syma_named("KV", syma_cat(syma_one(key_term),
                                                                         syma_one(value_term)))));
        }
        Id program = syma_section(x, "Program");
        if (!program)
            syma_fail("program arguments require a Program section");
        /* Compiling a Module can qualify its Args section. Preserve that
         * existing head so the module's rules still see the injected values. */
        Seq *parts = NULL;
        Count index;
        unsigned replaced = 0;
        for (index = 0; index < syma_argcnt(program); index++) {
            Id part = syma_arg(program, index);
            if (syma_node(part)->kind == CALL && syma_node(part)->h &&
                syma_node(syma_node(part)->h)->kind == SYM) {
                char *head = own(syma_utf8(syma_node(part)->h));
                if (!strcmp(head, "Args") || suffix(head, "/Args")) {
                    part = syma_call(syma_node(part)->h, values);
                    replaced = 1;
                }
            }
            parts = syma_cat(parts, syma_one(part));
        }
        if (!replaced)
            parts = syma_cat(parts, syma_one(syma_named("Args", values)));
        program = syma_call(syma_node(program)->h, parts);
        x = syma_replace_section(x, "Program", program);
        session.pending = x;
    }
    x = syma_rule_rules(x, engine.universe, &engine.rules);
    install(x);
    if (!bundle && syma_section(x, "Program"))
        run_program();
    if (remember) {
        free(session.reload);
        session.reload = syma_mem(((Count)strlen(original) + 10) * sizeof(char));
        strcpy(session.reload, bundle ? "bundle " : "load ");
        strcat(session.reload, original);
    }
    printf("Universe %s from %s\n", bundle ? "bundled" : "loaded", path);
}
static void import_library(char *source) {
    Id parsed = syma_parse(source);
    Seq *parts = syma_headis(parsed, "Bundle") ? syma_node(parsed)->a : syma_one(parsed);
    Id name = syma_at(parts, 0), alias = name, path = 0;
    unsigned open = 0, macro = 0;
    Count i;
    if (!name)
        syma_fail("usage: :import MODULE [as ALIAS] [from \"FILE\"] [open] [macro]");
    if (syma_node(name)->kind == STR || suffix(own(syma_utf8(name)), ".syma")) {
        char *filename = own(syma_utf8(name)), *text = own(syma_readfile(filename));
        Id ast = syma_parse(text);
        if (!syma_headis(ast, "Module"))
            syma_fail("import file must contain a Module");
        name = alias = syma_arg(ast, 0);
        path = syma_str(filename);
    }
    for (i = 1; i < syma_length(parts); i++) {
        Id option = syma_at(parts, i);
        if (syma_is(option, "open"))
            open = 1;
        else if (syma_is(option, "macro"))
            macro = 1;
        else if (syma_is(option, "as") && i + 1 < syma_length(parts))
            alias = syma_at(parts, ++i);
        else if (syma_is(option, "from") && i + 1 < syma_length(parts))
            path = syma_at(parts, ++i);
        else
            syma_fail("unknown import modifier");
    }
    Seq *request = syma_one(name);
    if (alias != name)
        request = syma_cat(request, syma_cat(syma_one(syma_sym("as")), syma_one(alias)));
    if (path)
        request = syma_cat(request, syma_cat(syma_one(syma_sym("from")), syma_one(path)));
    if (open)
        request = syma_cat(request, syma_one(syma_sym("open")));
    if (macro)
        request = syma_cat(request, syma_one(syma_sym("macro")));
    /* A module already loaded by :module may have RuleRules without a disk file. */
    Id rr = syma_section(engine.universe, "RuleRules"), tag = syma_str(own(syma_utf8(name)));
    unsigned found = 0;
    for (i = 0; i < syma_argcnt(rr); i++)
        if (same_name(syma_arg(syma_arg(rr, i), 0), tag))
            found = 1;
    if (!(found && macro && !open && alias == name && !path)) {
        Id library = syma_import_library(syma_named("Import", request));
        Id use = syma_named("Use", syma_cat(syma_cat(syma_cat(syma_one(name), syma_one(alias)),
                                                     syma_one(syma_arg(library, 1))),
                                            syma_one(syma_sym(open ? "True" : "False"))));
        session.pending = syma_named("ImportKeep", syma_cat(syma_one(library), syma_one(use)));
        Id imported = syma_replace_section(syma_arg(library, 0), "ReplImports",
                                           syma_named("ReplImports", syma_one(use)));
        merge_universe(imported);
    }
    if (macro) {
        Id old = syma_section(engine.universe, "ReplMacros");
        Seq *a = old ? syma_node(old)->a : NULL;
        for (i = 0; i < syma_length(a); i++)
            if (syma_at(a, i) == tag)
                break;
        if (i == syma_length(a))
            a = syma_cat(a, syma_one(tag));
        engine.universe =
            syma_replace_section(engine.universe, "ReplMacros", syma_named("ReplMacros", a));
    }
    puts("Module imported");
}
/* Split command clauses only outside strings/comments/delimiters. */
static char *clause(char *source, char **next) {
    char *p = source;
    long depth = 0;
    unsigned quote = 0, raw = 0, escape = 0, comment = 0, block = 0;
    for (; *p; p++) {
        if (comment) {
            if (*p == '\n')
                comment = 0;
            continue;
        }
        if (block) {
            if (p[0] == '*' && p[1] == '/') {
                block = 0;
                p++;
            }
            continue;
        }
        if (quote) {
            if (escape)
                escape = 0;
            else if (!raw && *p == '\\')
                escape = 1;
            else if (*p == '"')
                quote = 0;
            continue;
        }
        if (*p == '"') {
            quote = 1;
            raw = p > source && p[-1] == 'r';
        } else if (*p == ';' || (p[0] == '/' && p[1] == '/'))
            comment = 1;
        else if (p[0] == '/' && p[1] == '*') {
            block = 1;
            p++;
        } else if (*p == '{' || *p == '(')
            depth++;
        else if (*p == '}' || *p == ')')
            depth--;
        else if (!depth && *p == ':' && (p == source || isspace((unsigned char)p[-1]))) {
            *next = p;
            return p;
        }
    }
    *next = NULL;
    return p;
}
static void match_command(char *source) {
    char *next, *end = clause(source, &next), *rewrite = NULL, *target_source = NULL;
    unsigned norm = 0, universe_mode = 0, against_universe = 1;
    if (next) {
        *end = 0;
        next++;
    }
    char *pattern_source = trim(source);
    if (!*pattern_source)
        syma_fail("match requires a pattern");
    while (next) {
        char *kind = word(&next), *body = next;
        end = clause(body, &next);
        if (next) {
            *end = 0;
            next++;
        }
        body = trim(body);
        if (!strcmp(kind, "rewrite"))
            rewrite = body;
        else if (!strcmp(kind, "target") || !strcmp(kind, "norm")) {
            target_source = body;
            norm = !strcmp(kind, "norm");
            against_universe = 0;
        } else if (!strcmp(kind, "universe"))
            universe_mode = 1;
        else
            syma_fail("unknown match clause");
    }
    Id pattern = expression(pattern_source), target,
       replacement = rewrite ? expression(rewrite) : 0;
    const char *section_name = NULL;
    if (!against_universe && universe_mode)
        syma_fail("choose :target, :norm or :universe");
    if (!against_universe) {
        if (!target_source || !*target_source)
            syma_fail("match target is required");
        target = expression(target_source);
        if (norm)
            target = normalize(target, session.automatic_trace ? session.trace_style : 0);
    } else if (universe_mode) {
        if (syma_headis(pattern, "Universe"))
            target = engine.universe;
        else {
            if (syma_headis(pattern, "Program"))
                section_name = "Program";
            else if (syma_headis(pattern, "Rules"))
                section_name = "Rules";
            else if (syma_headis(pattern, "RuleRules"))
                section_name = "RuleRules";
            else if (syma_headis(pattern, "MacroScopes"))
                section_name = "MacroScopes";
            else
                section_name = "Program";
            target = syma_section(engine.universe, section_name);
        }
    } else {
        target = engine.universe;
        if (!syma_headis(pattern, "Universe")) {
            Seq *fragments =
                syma_headis(pattern, "Bundle") ? syma_node(pattern)->a : syma_one(pattern);
            pattern = syma_named(
                "Universe",
                syma_cat(fragments, syma_one(syma_named("VarRest", syma_one(syma_str("_"))))));
        }
    }
    Id bindings = 0, result = 0;
    if (!target || !syma_match_term(pattern, target, replacement, &bindings, &result)) {
        puts("No match");
        return;
    }
    puts("Match succeeded");
    output(bindings);
    if (replacement) {
        output(result);
        if (universe_mode) {
            Id u =
                section_name ? syma_replace_section(engine.universe, section_name, result) : result;
            if (section_name && (!syma_node(result) || syma_node(result)->kind != CALL ||
                                 !syma_headis(result, section_name)))
                syma_fail("section rewrite must preserve its section head");
            install(syma_rule_rules(u, engine.universe, &engine.rules));
        }
    }
}
static void why(char *source) {
    Id x = expression(source), result;
    Count i, candidates = 0;
    for (i = 0; i < engine.rules.n; i++) {
        Rule *r = &engine.rules.v[i];
        if (syma_match_term(r->lhs, x, 0, NULL, NULL)) {
            candidates++;
            printf("%s: ", own(syma_utf8(r->name)));
            if (syma_probe_rule(&engine, i, x, &result))
                puts("matches (rule can rewrite this expression)");
            else if (r->scope)
                puts("scope requires a matching ancestor");
            else if (r->with || r->let)
                puts("guard or context condition does not match");
            else if (r->guard)
                puts("guard is not True");
            else
                puts("condition does not match");
        } else if (syma_node(x)->kind == CALL && syma_node(r->lhs)->kind == CALL &&
                   syma_node(x)->h == syma_node(r->lhs)->h) {
            candidates++;
            printf("%s: argument pattern does not match\n", own(syma_utf8(r->name)));
        }
    }
    if (!candidates)
        puts("No candidate rules at the expression root");
    Id folded = syma_fold(&engine, x, 0);
    if (folded != x) {
        puts("A primitive can reduce this expression:");
        output(folded);
    }
}
static void apply(char *source, unsigned execute) {
    char *rest = source, *name = word(&rest);
    Count i;
    for (i = 0; i < engine.rules.n; i++)
        if (same_name(engine.rules.v[i].name, syma_str(name)))
            break;
    if (*rest && i < engine.rules.n) {
        Rule *r = &engine.rules.v[i];
        Id x = expression(rest), y;
        if (execute) {
            if (!syma_match_term(r->lhs, x, 0, NULL, NULL)) {
                if (syma_node(r->lhs)->kind != CALL)
                    syma_fail("cannot wrap expression for this rule");
                x = syma_call(syma_node(r->lhs)->h, syma_one(x));
            }
            output(normalize(x, session.automatic_trace ? session.trace_style : 0));
        } else if (syma_probe_rule(&engine, i, x, &y)) {
            session.last = y;
            output(y);
        } else
            puts("Rule does not match expression or conditions");
        return;
    }
    if (execute)
        syma_fail("usage: :exec RULE EXPRESSION");
    Id action = expression(source), program = syma_section(engine.universe, "Program");
    if (!program)
        syma_fail("no Program section for action");
    if (syma_node(action)->kind == SYM) {
        char *text = own(syma_utf8(action));
        Count n = (Count)strlen(text);
        for (i = 0; i < engine.rules.n; i++) {
            Rule *r = &engine.rules.v[i];
            char *rn = own(syma_utf8(r->name));
            Count len = (Count)strlen(rn);
            if ((!strcmp(rn, text) ||
                 (len > n && rn[len - n - 1] == '/' && !strcmp(rn + len - n, text))) &&
                syma_headis(r->lhs, "Apply")) {
                action = syma_arg(r->lhs, 0);
                break;
            }
        }
    }
    Id applied = syma_named("Apply", syma_cat(syma_one(action), syma_one(program)));
    program = normalize(applied, session.automatic_trace ? session.trace_style : 0);
    if (syma_headis(program, "App"))
        program = syma_named("Program", syma_one(program));
    if (!syma_headis(program, "Program"))
        syma_fail("action did not produce a Program");
    engine.universe = syma_replace_section(engine.universe, "Program", program);
    run_program();
    section("Program");
}
static void help(void) {
    puts("Syma REPL\n"
         "  EXPR                     Evaluate; $it is the last result\n"
         "  :universe/:u  :program/:p  :rules-section/:rs  :rulerules/:rr\n"
         "  :rules  :rule NAME  :rule NAME PATTERN -> REPLACEMENT\n"
         "  :rule multiline / :end   :module multiline / :end\n"
         "  :rule / .                Define a full R term on multiple lines\n"
         "  :edit NAME PATTERN -> REPLACEMENT   :drop NAME\n"
         "  :match/:m PATTERN [:rewrite REPLACEMENT] [:target EXPR|:norm EXPR|:universe]\n"
         "  :match / :end            Multiline matching\n"
         "  :apply ACTION  :apply RULE EXPR  :exec RULE EXPR  :norm [show]\n"
         "  :trace [EXPR]  :trace verbose|diff [EXPR]  :trace stats EXPR\n"
         "  :why EXPR  :macro-scopes\n"
         "  :save FILE  :load FILE  :bundle FILE [:args --KEY VALUE]  :reload\n"
         "  :import MODULE|FILE [as ALIAS] [from \"FILE\"] [open] [macro]\n"
         "  :export MODULE FILE      Save a compiled module library\n"
         "  :undo  :history [N]  :clear\n"
         "  :set trace on|off  :set maxsteps N\n"
         "  :help/:h  :quit/:q/:exit\n"
         "Balanced input continues automatically; EXPR \\ starts input ending with .");
}
static void export_library(char *source) {
    char *name = word(&source), *path = word(&source);
    if (!*name || !*path)
        syma_fail("usage: :export MODULE FILE");
    Seq *rules = NULL;
    Count i;
    Id old = syma_section(engine.universe, "Rules");
    for (i = 0; i < syma_argcnt(old); i++) {
        Id r = syma_arg(old, i);
        char *rn = own(syma_utf8(rule_name(r)));
        if ((syma_headis(r, "TaggedRule") && same_name(syma_arg(r, 0), syma_str(name))) ||
            (!strncmp(rn, name, strlen(name)) && rn[strlen(name)] == '/'))
            rules = syma_cat(rules, syma_one(r));
    }
    if (!rules)
        syma_fail("no rules found for module");
    /* Dependencies and RuleRules remain available in the exported library. */
    save_file(path, syma_replace_section(engine.universe, "Program", syma_named("Program", NULL)));
    puts("Compiled library exported (includes dependencies)");
}
static int command(char *source) {
    char *name = word(&source), *arg;
    Count lower;
    for (lower = 0; name[lower]; lower++)
        name[lower] = (char)tolower((unsigned char)name[lower]);
    if (!strcmp(name, "quit") || !strcmp(name, "q") || !strcmp(name, "exit"))
        return 0;
    if (!strcmp(name, "help") || !strcmp(name, "h"))
        help();
    else if (!strcmp(name, "universe") || !strcmp(name, "u"))
        output(engine.universe);
    else if (!strcmp(name, "program") || !strcmp(name, "p"))
        section("Program");
    else if (!strcmp(name, "rules-section") || !strcmp(name, "rs"))
        section("Rules");
    else if (!strcmp(name, "rulerules") || !strcmp(name, "rr"))
        section("RuleRules");
    else if (!strcmp(name, "macro-scopes"))
        section("MacroScopes");
    else if (!strcmp(name, "rules")) {
        Count i;
        printf("Rules (%lu):\n", (unsigned long)engine.rules.n);
        for (i = 0; i < engine.rules.n; i++)
            printf("  %s [%.15g]\n", own(syma_utf8(engine.rules.v[i].name)),
                   engine.rules.v[i].prio);
    } else if (!strcmp(name, "clear")) {
        syma_effects_cleanup();
        install(empty_universe());
        puts("Universe cleared");
    } else if (!strcmp(name, "undo")) {
        if (!session.undos)
            syma_fail("nothing to undo");
        install(session.undo[--session.undos]);
        session.transaction = engine.universe;
        syma_effects_cleanup();
        puts("Last modification undone");
    } else if (!strcmp(name, "history")) {
        Count n = *source ? (Count)strtoul(source, NULL, 10) : 20, i;
        if (n > session.history_count)
            n = session.history_count;
        for (i = session.history_count - n; i < session.history_count; i++)
            printf("%lu: %s\n", (unsigned long)i + 1, session.history[i]);
    } else if (!strcmp(name, "set")) {
        arg = word(&source);
        if (!strcmp(arg, "maxsteps")) {
            Count n = (Count)strtoul(source, NULL, 10);
            if (!n)
                syma_fail("maxsteps must be positive");
            engine.maxsteps = n;
        } else if (!strcmp(arg, "trace") && (!strcmp(source, "on") || !strcmp(source, "off")))
            session.automatic_trace = !strcmp(source, "on");
        else
            syma_fail("usage: :set maxsteps N / :set trace on|off");
    } else if (!strcmp(name, "rule") || !strcmp(name, "edit")) {
        if (!*source || !strcmp(source, "multiline"))
            session.mode = !strcmp(source, "multiline") ? 1 : 4;
        else {
            char *body = source;
            arg = word(&body);
            if (!*body)
                output(engine.rules.v[find_rule(arg)].ast);
            else {
                if (!strcmp(name, "edit"))
                    (void)find_rule(arg);
                add_rule(inline_rule(source), !strcmp(name, "edit"));
                puts("Rule added");
            }
        }
    } else if (!strcmp(name, "drop")) {
        arg = word(&source);
        drop_rule(arg);
        puts("Rule removed");
    } else if (!strcmp(name, "module") && !strcmp(source, "multiline"))
        session.mode = 2;
    else if (!strcmp(name, "match") || !strcmp(name, "m")) {
        if (!*source)
            session.mode = 3;
        else
            match_command(source);
    } else if (!strcmp(name, "why"))
        why(source);
    else if (!strcmp(name, "apply") || !strcmp(name, "exec"))
        apply(source, !strcmp(name, "exec"));
    else if (!strcmp(name, "norm")) {
        run_program();
        puts("Universe normalized");
        if (!strcmp(source, "show"))
            section("Program");
    } else if (!strcmp(name, "load") || !strcmp(name, "bundle"))
        load_file(source, !strcmp(name, "bundle"), 1);
    else if (!strcmp(name, "reload")) {
        if (!session.reload)
            syma_fail("no previous load or bundle");
        char *op = copy(session.reload);
        char *kind = word(&op);
        load_file(op, !strcmp(kind, "bundle"), 0);
    } else if (!strcmp(name, "save")) {
        arg = word(&source);
        if (!*arg)
            syma_fail("usage: :save FILE");
        save_file(arg, engine.universe);
        puts("Universe saved");
    } else if (!strcmp(name, "import"))
        import_library(source);
    else if (!strcmp(name, "export"))
        export_library(source);
    else if (!strcmp(name, "trace")) {
        unsigned style = session.trace_style;
        if (!*source) {
            session.automatic_trace = !session.automatic_trace;
            printf("Trace: %s\n", session.automatic_trace ? "on" : "off");
            return 1;
        }
        char *rest = source;
        arg = word(&rest);
        if (!strcmp(arg, "verbose") || !strcmp(arg, "diff") || !strcmp(arg, "stats")) {
            style = !strcmp(arg, "verbose") ? 3 : !strcmp(arg, "diff") ? 5 : 8;
            if (!*rest) {
                if (style == 8)
                    syma_fail("usage: :trace stats EXPR");
                session.trace_style = session.trace_style == style ? 1 : style;
                session.automatic_trace = 1;
                return 1;
            }
            source = rest;
        }
        output(normalize(expression(source), style));
    } else
        syma_fail("unknown command; use :help");
    return 1;
}

static int balanced(const char *s) {
    long depth = 0;
    unsigned quote = 0, raw = 0, escape = 0, comment = 0, block = 0;
    Count i;
    for (i = 0; s[i]; i++) {
        unsigned c = (unsigned char)s[i];
        if (comment) {
            if (c == '\n')
                comment = 0;
            continue;
        }
        if (block) {
            if (c == '*' && s[i + 1] == '/') {
                block = 0;
                i++;
            }
            continue;
        }
        if (quote) {
            if (escape)
                escape = 0;
            else if (!raw && c == 92)
                escape = 1;
            else if (c == '"')
                quote = 0;
            continue;
        }
        if (c == ';' || (c == '/' && s[i + 1] == '/')) {
            comment = 1;
            continue;
        }
        if (c == '/' && s[i + 1] == '*') {
            block = 1;
            i++;
            continue;
        }
        if (c == '"') {
            quote = 1;
            raw = i && s[i - 1] == 'r';
        } else if (c == '{' || c == '(')
            depth++;
        else if (c == '}' || c == ')')
            depth--;
    }
    return depth <= 0 && !quote && !block;
}
static int empty_source(const char *p) {
    while (*p) {
        if (*p == 32 || *p == 9 || *p == 10 || *p == 13) {
            p++;
            continue;
        }
        if (*p == 59 || (p[0] == 47 && p[1] == 47)) {
            while (*p && *p != 10)
                p++;
            continue;
        }
        if (p[0] == 47 && p[1] == 42) {
            p += 2;
            while (*p && !(p[0] == 42 && p[1] == 47))
                p++;
            if (!*p)
                return 0;
            p += 2;
            continue;
        }
        return 0;
    }
    return 1;
}
static void history(char *source) {
    Count i, n = (Count)strlen(source), bytes = n;
    if (n > 65536)
        return;
    for (i = 0; i < session.history_count; i++)
        bytes += (Count)strlen(session.history[i]);
    while (session.history_count && (session.history_count == 200 || bytes > 65536)) {
        bytes -= (Count)strlen(session.history[0]);
        free(session.history[0]);
        memmove(session.history, session.history + 1, (--session.history_count) * sizeof(char *));
    }
    char *entry = syma_mem((n + 1) * sizeof(char));
    strcpy(entry, source);
    session.history[session.history_count++] = entry;
}
static void history_file(unsigned save) {
    const char *home = getenv("HOME");
    if (!home || !*home)
        home = ".";
    char *directory = temporary(((Count)strlen(home) + 8) * sizeof(char));
    strcpy(directory, home);
    strcat(directory, "/.syma");
    char *path = temporary(((Count)strlen(directory) + 16) * sizeof(char));
    strcpy(path, directory);
    strcat(path, "/history.syma");
    if (save) {
        (void)mkdir(directory, 0700);
        FILE *file = fopen(path, "w");
        if (!file) {
            fprintf(stderr, "syma: cannot save REPL history\n");
            return;
        }
        if (chmod(path, 0600)) {
            fclose(file);
            fprintf(stderr, "syma: cannot protect REPL history\n");
            return;
        }
        Seq *a = NULL;
        Count i;
        for (i = 0; i < session.history_count; i++)
            a = syma_cat(a, syma_one(syma_str(session.history[i])));
        char *text = own(syma_show(syma_named("History", a), 0));
        int ok = fputs(text, file) >= 0;
        if (fclose(file))
            ok = 0;
        if (!ok)
            fprintf(stderr, "syma: cannot write REPL history\n");
    } else {
        FILE *file = fopen(path, "r");
        if (!file)
            return;
        Buffer data = {0};
        int ch;
        while ((ch = fgetc(file)) != EOF)
            syma_bufput(&data, (unsigned)ch);
        fclose(file);
        char *text = own(data.p);
        jmp_buf local;
        jmp_buf *previous = syma_error_target;
        syma_error_target = &local;
        if (!setjmp(local)) {
            Id saved = syma_parse(text ? text : "");
            Count i;
            if (!syma_headis(saved, "History"))
                syma_fail("invalid history file");
            for (i = 0; i < syma_argcnt(saved); i++) {
                Id item = syma_arg(saved, i);
                if (!item || syma_node(item)->kind != STR)
                    syma_fail("invalid history entry");
                char *entry = syma_utf8(item);
                history(entry);
                free(entry);
            }
        } else
            fprintf(stderr, "syma: ignoring malformed REPL history\n");
        syma_error_target = previous;
    }
}
static void evaluate(char *source) {
    Id x = syma_parse(source);
    if (syma_headis(x, "R"))
        add_rule(x, 0);
    else if (syma_headis(x, "Module"))
        merge_universe(syma_compile_module(x, "./repl.syma"));
    else if (syma_headis(x, "Universe"))
        install(enrich(syma_rule_rules(x, engine.universe, &engine.rules)));
    else
        output(normalize(qualify(x, 0), session.automatic_trace ? session.trace_style : 0));
}
int syma_repl(Engine *target) {
    char line[1024], control[1024];
    jmp_buf local;
    jmp_buf *previous = syma_error_target;
    unsigned mode;
    memset(&session, 0, sizeof(session));
    session.vm = target;
    session.interactive = isatty(0);
    session.trace_style = 1;
    setvbuf(stdin, NULL, _IONBF, 0);
    if (!engine.universe)
        install(empty_universe());
    if (session.interactive) {
        history_file(0);
        clear_temporary();
        puts("Syma REPL — :help for commands, :quit to exit");
    }
    for (;;) {
        syma_error_target = &local;
        if (setjmp(local)) {
            session.failures++;
            fprintf(stderr, "syma: %s (line %lu)\n", syma_message, (unsigned long)session.lines);
            clear_temporary();
            syma_effects_cleanup();
            syma_modules_clear();
            syma_match_cleanup();
            syma_visit_cleanup();
            if (session.transaction)
                install(session.transaction);
            session.transaction = 0;
            session.mode = engine.trace = 0;
            session.input.n = 0;
            if (session.input.p)
                session.input.p[0] = 0;
            syma_gc(0, engine.universe, &engine.rules, NULL, 0);
        }
        if (session.interactive) {
            fputs(session.input.n || session.mode ? "... " : "syma> ", stdout);
            fflush(stdout);
        }
        if (!fgets(line, sizeof(line), stdin))
            break;
        session.lines++;
        strcpy(control, line);
        char *input = trim(control);
        /* Historical Maze notebook begins an empty multiline block twice. */
        if (session.mode == 1 && !strcmp(input, ":rule multiline") &&
            empty_source(session.input.p ? session.input.p : "")) {
            session.input.n = 0;
            if (session.input.p)
                session.input.p[0] = 0;
            continue;
        }
        Count control_len = (Count)strlen(input);
        if (!session.mode && !session.input.n && control_len && input[control_len - 1] == '\\') {
            input[control_len - 1] = 0;
            session.mode = 5;
            syma_buftext(&session.input, input);
            syma_bufput(&session.input, '\n');
            continue;
        }
        unsigned end = session.mode && ((!strcmp(input, ":end") && session.mode <= 3) ||
                                        (!strcmp(input, ".") && session.mode >= 4));
        if (!end) {
            syma_buftext(&session.input, line);
            /* trim removed the newline, so retain physical line boundaries. */
            if (strchr(line, '\n') == NULL && strlen(line) < sizeof(line) - 1)
                syma_bufput(&session.input, '\n');
            if (session.input.n > 1048576UL)
                syma_fail("REPL input exceeds 1 MiB");
            if (strlen(line) && line[strlen(line) - 1] != '\n' && !feof(stdin))
                continue;
            if (session.mode || !balanced(session.input.p))
                continue;
        }
        mode = session.mode;
        session.mode = 0;
        char *source = trim(session.input.p ? session.input.p : "");
        if (empty_source(source)) {
            session.input.n = 0;
            if (session.input.p)
                session.input.p[0] = 0;
            continue;
        }
        session.transaction = engine.universe;
        history(source);
        source = copy(source);
        int keep = 1;
        if (mode == 1 || mode == 4) {
            Id ast = source[0] == '{' || !strncmp(source, "R(", 2) ? syma_parse(source)
                                                                   : inline_rule(source);
            if (!syma_headis(ast, "R"))
                syma_fail("expected R rule definition");
            add_rule(ast, 0);
        } else if (mode == 2) {
            Id ast = syma_parse(source);
            if (!syma_headis(ast, "Module"))
                syma_fail("expected Module declaration");
            merge_universe(syma_compile_module(ast, "./repl.syma"));
        } else if (mode == 3)
            match_command(source);
        else if (source[0] == ':')
            keep = command(source + 1);
        else {
            Count n = (Count)strlen(source);
            if (!mode && n && source[n - 1] == '\\') {
                source[n - 1] = 0;
                session.mode = 5;
                session.input.n = 0;
                session.input.p[0] = 0;
                syma_buftext(&session.input, source);
                syma_bufput(&session.input, '\n');
                clear_temporary();
                continue;
            }
            evaluate(source);
        }
        if (engine.universe != session.transaction)
            checkpoint();
        session.transaction = 0;
        clear_temporary();
        session.input.n = 0;
        if (session.input.p)
            session.input.p[0] = 0;
        syma_gc(0, engine.universe, &engine.rules, NULL, 0);
        if (!keep)
            break;
    }
    if (session.input.n || session.mode) {
        session.failures++;
        fprintf(stderr, "syma: incomplete input at end of file\n");
    }
    int status = !session.interactive && session.failures ? 1 : 0;
    syma_error_target = previous;
    if (session.interactive)
        history_file(1);
    clear_temporary();
    syma_effects_cleanup();
    Count i;
    for (i = 0; i < session.history_count; i++)
        free(session.history[i]);
    free(session.reload);
    free(session.input.p);
    memset(&session, 0, sizeof(session));
    return status;
}
