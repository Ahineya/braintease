/* Syma's portable C16 runtime. Indices and lengths never use C16 int. */
#ifndef SYMA_H
#define SYMA_H
#include <math.h>
#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef uint32_t Id;
typedef uint32_t Count;
typedef uint16_t Unit;
enum { SYM = 1, NUM, STR, CALL, SPLICE };
typedef struct Seq Seq;
struct Seq {
    Seq *next, *l, *r;
    Count n;
    uint32_t hash, power;
    unsigned height, mark, used, flags;
    Id items[8];
};
typedef struct {
    unsigned kind, mark, flags, primitive;
    uint32_t hash;
    Id link, h;
    Count len;
    Unit *s;
    double num;
    Seq *a;
} Node;
typedef struct {
    Id name, lhs, rhs, guard, scope, with, let, ast, key;
    double prio;
    unsigned inner, universal;
    Count order, next;
} Rule;
typedef struct {
    Rule *v;
    Count n, cap;
    Count *buckets, bucketn;
    unsigned hasinner, atom_types;
} Rules;
typedef struct {
    Rules rules;
    Id universe, keep;
    Count maxsteps, steps;
    unsigned preserve, meta, skipprims, trace, depth;
    Count rewrites, primitive_steps;
    Count *trace_counts;
    Count excluded;
    Count excluded_count, excluded_rules[32];
} Engine;
typedef struct {
    char *p;
    Count n, cap;
} Buffer;
extern jmp_buf syma_error;
extern jmp_buf *syma_error_target;
extern char syma_message[256];
void syma_fail(const char *msg);
void *syma_mem(Count n);
void *syma_resize(void *p, Count n);
Node *syma_node(Id id);
/* C16 does not currently inline across objects. Keep this hot, read-only table
 * access out of the process call ABI. All arguments in this runtime are pure
 * ID expressions (no increment, allocation or evaluation side effects). */
extern Node *syma_pages[4096];
#ifndef SYMA_TERM_IMPLEMENTATION
#define syma_node(id) ((id) ? &syma_pages[((Id)(id)) >> 8][((Id)(id)) & 255UL] : NULL)
#endif

Id syma_atom(unsigned kind, const Unit *s, Count n);
Id syma_sym(const char *s);
Id syma_str(const char *s);
Id syma_number(double n);
Id syma_call(Id h, Seq *a);
Id syma_named(const char *h, Seq *a);
int syma_is(Id x, const char *s);
int syma_headis(Id x, const char *s);
Id syma_arg(Id x, Count i);
Count syma_argcnt(Id x);
Seq *syma_one(Id x);
Seq *syma_cat(Seq *a, Seq *b);
Seq *syma_slice(Seq *s, Count from, Count n);
Id syma_at(Seq *s, Count i);
Seq *syma_flat(Id x);
Id syma_fromflat(Seq *s);
Count syma_length(Seq *s);
char *syma_utf8(Id x);
Id syma_utf8atom(unsigned kind, const char *s, Count n);
void syma_bufput(Buffer *b, unsigned c);
void syma_buftext(Buffer *b, const char *s);
char *syma_show(Id x, unsigned json);
Id syma_parse(const char *text);
Id syma_inline_rule(char *text);
Id syma_compile_module(Id ast, const char *path);
Id syma_jsonparse(const char *text);
void syma_store_init(void);
void syma_store_free(void);
void syma_match_cleanup(void);
void syma_visit_cleanup(void);
void syma_gc(Id root, Id universe, Rules *rules, Id *pins, Count npins);
Count syma_live_nodes(void);
Count syma_allocated_sequences(void);
double syma_now(void);
double syma_random(void);
void syma_rules_init(Rules *r);
void syma_rules_free(Rules *r);
void syma_rules_add(Rules *r, Id ast);
void syma_rules_extract(Rules *r, Id universe);
void syma_rules_index(Rules *r);
Id syma_normalize(Engine *e, Id x);
Id syma_fold(Engine *e, Id x, unsigned meta);
Id syma_primitive(Engine *e, Id x, unsigned meta);
unsigned syma_primitive_id(const Unit *text, Count n);
unsigned syma_tag(Id term);
enum {
    SYMA_T_VAR = 1,
    SYMA_T_REST,
    SYMA_T_GREEDY,
    SYMA_T_META_MATCH,
    SYMA_T_ALT,
    SYMA_T_NOT,
    SYMA_T_REPEAT,
    SYMA_T_REGEX,
    SYMA_T_INERT,
    SYMA_T_META_SAFE,
    SYMA_T_QUOTE,
    SYMA_T_UNBOUND,
    SYMA_T_SPLICE
};
extern const unsigned syma_primitive_info[];
Id syma_rule_rules(Id universe, Id retained, Rules *retained_rules);
void syma_mark_effects(void (*mark)(Id));
Id syma_section(Id x, const char *name);
Id syma_replace_section(Id x, const char *name, Id replacement);
char *syma_readfile(const char *name);
Id syma_load_module(const char *path);
void syma_library_hint(const char *executable);
void syma_modules_clear(void);
Id syma_run_effects(Engine *e, Id program, unsigned *exited);
void syma_effects_cleanup(void);
int syma_effect_status(void);
int syma_main(int ac, char **av);
int syma_repl(Engine *engine);
void syma_mark_repl(void (*mark)(Id));
void syma_mark_contexts(void (*mark)(Id));
int syma_worksheet(int ac, char **av);
Id syma_import_library(Id request);
int syma_match_term(Id pattern, Id subject, Id replacement, Id *bindings, Id *result);
int syma_probe_rule(Engine *engine, Count index, Id subject, Id *result);
#endif
