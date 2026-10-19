/* SPDX-License-Identifier: MIT
 * Same vectors against pristine host and adapted C16 Unicode/regexp engines.
 * No JavaScript interpreter or substitute regexp implementation is involved.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include "cutils.h"
#include "libunicode.h"
#include "libregexp.h"
#if CHAR_BIT == 16
typedef long QInt;
typedef qjs_octet_t Octet;
#else
typedef int QInt;
typedef uint8_t Octet;
#endif
static unsigned checks, failures;
static uint32_t digest = 2166136261UL;
static void check(int ok, const char *label)
{ ++checks; if (!ok) { ++failures; printf("FAIL %s\n", label); } }
static void mix(uint32_t value)
{
    unsigned i;
    for (i = 0; i < 4; ++i) { digest = (digest ^ (value & 255)) * 16777619UL; value >>= 8; }
}
struct Allocator {
    size_t calls, fail_at, live, blocks, maximum;
    uintptr_t stack_base;
    size_t stack_limit;
    unsigned polls, timeout_after, reject_stack, stack_trips;
};
typedef union Header {
    struct { size_t size; uint32_t magic; } data;
    double alignment;
    void *pointer_alignment;
} Header;
static uintptr_t linear(uintptr_t pointer)
{
#if CHAR_BIT == 16
    return (pointer >> 16) * 64000UL + (pointer & 65535UL);
#else
    return pointer;
#endif
}
static void *tracked_realloc(void *opaque, void *ptr, size_t size)
{
    struct Allocator *a = opaque;
    Header *old = ptr ? (Header *)ptr - 1 : NULL, *fresh;
    size_t previous = old ? old->data.size : 0;
    if (old && (old->data.magic != 0x716a7361UL || ((Octet *)ptr)[previous] != 0x5a)) {
        fputs("FAIL allocator guard\n", stderr); abort();
    }
    if (size) {
        ++a->calls;
        if (a->fail_at && a->calls == a->fail_at) return NULL;
        if (size > (size_t)-1 - sizeof(Header) - 1) return NULL;
        fresh = malloc(sizeof(Header) + size + 1);
        if (!fresh) return NULL;
        fresh->data.size = size; fresh->data.magic = 0x716a7361UL;
        ((Octet *)(fresh + 1))[size] = 0x5a;
        if (old) memcpy(fresh + 1, ptr, previous < size ? previous : size);
        else ++a->blocks;
    } else {
        fresh = NULL;
        if (old) --a->blocks;
    }
    if (old) free(old);
    a->live = a->live - previous + size;
    if (a->live > a->maximum) a->maximum = a->live;
    return fresh ? fresh + 1 : NULL;
}
void *lre_realloc(void *opaque, void *ptr, size_t size)
{ return tracked_realloc(opaque, ptr, size); }
QInt lre_check_stack_overflow(void *opaque, size_t requested)
{
    struct Allocator *a = opaque;
    unsigned local;
    uintptr_t here = linear((uintptr_t)&local);
    size_t distance = here < a->stack_base ? a->stack_base - here : here - a->stack_base;
    if (a->reject_stack || distance >= a->stack_limit || requested > a->stack_limit - distance) {
        ++a->stack_trips; return 1;
    }
    return 0;
}
QInt lre_check_timeout(void *opaque)
{
    struct Allocator *a = opaque;
    ++a->polls;
    return a->timeout_after && a->polls >= a->timeout_after;
}
static int contains(const CharRange *range, uint32_t code)
{
    QInt i;
    for (i = 0; i < range->len; i += 2)
        if (range->points[i] <= code && code < range->points[i+1]) return 1;
    return 0;
}
static void sequence(void *opaque, const uint32_t *points, QInt length)
{
    uint32_t *count = opaque;
    QInt i;
    ++*count; mix(length);
    for (i = 0; i < length; ++i) mix(points[i]);
}
static void unicode_tests(struct Allocator *a)
{
    uint32_t cp, result[3], *normalized, count = 0;
    QInt n, i, mode;
    unsigned j;
    CharRange range;
    static const uint32_t cases[][8] = {
        {0x65,0x301}, {0x212b}, {0xac01}, {0x1100,0x1161,0x11a8},
        {0xfb03}, {0x1d15e}, {0x1e0a,0x323}, {0x1e9b,0x323},
        {0x1f82}, {0x1e4ec}, {0x1d400}, {0x2460}, {0x0344}, {0x41,0x315,0x300}
    };
    static const unsigned lengths[] = {2,1,1,3,1,1,2,2,1,1,1,1,1,3};
    static const char *properties[] = {"Alphabetic","Assigned","XID_Start","XID_Continue",
        "Uppercase","Lowercase","Math","Grapheme_Extend","Emoji","Any"};
    check(lre_case_conv(result, 0x457, 0) == 1 && result[0] == 0x407, "Ukrainian case conversion");
    check(lre_case_conv(result, 0xdf, 0) == 2 && result[0] == 'S' && result[1] == 'S', "multi-codepoint case conversion");
    check(lre_canonicalize(0x212a, 1) == 'k' && lre_canonicalize(0x212a, 0) == 0x212a,
          "Unicode versus legacy regexp case folding");
    check(lre_js_is_ident_first(0x10400UL) && lre_js_is_ident_next(0x200d), "supplementary identifier and joiner");
    /* Exhaust ASCII/Latin1, then sample every Unicode plane, including invalid
     * scalar values. Reference records expose table/index/sign-extension bugs. */
    for (cp = 0; cp <= 0x110100UL; cp += cp < 256 ? 1 : 97) {
        mix(cp); mix(lre_is_id_start(cp) != 0); mix(lre_is_id_continue(cp) != 0);
        mix(lre_is_cased(cp) != 0); mix(lre_is_case_ignorable(cp) != 0);
        mix(lre_canonicalize(cp, 0)); mix(lre_canonicalize(cp, 1));
        for (mode = 0; mode < 3; ++mode) {
            n = lre_case_conv(result, cp, mode); mix(n);
            for (i = 0; i < n; ++i) mix(result[i]);
        }
    }
    printf("U:classify:%08" PRIx32 "\n", digest);
    for (j = 0; j < sizeof(cases)/sizeof(cases[0]); ++j) {
        for (mode = 0; mode < 4; ++mode) {
            n = unicode_normalize(&normalized, cases[j], lengths[j], mode, a, tracked_realloc);
            check(n >= 0 && normalized != NULL, "normalization vector");
            mix(n);
            for (i = 0; i < n; ++i) mix(normalized[i]);
            tracked_realloc(a, normalized, 0);
        }
    }
    {
        uint32_t *input = malloc(18000UL * sizeof(*input));
        int exact = 1;
        if (!input) abort();
        for (i = 0; i < 18000L; ++i) input[i] = 0xe9;
        n = unicode_normalize(&normalized, input, 18000, UNICODE_NFD, a, tracked_realloc);
        if (n != 36000L || !normalized) exact = 0;
        else for (i = 0; i < n; ++i) {
            if (normalized[i] != (i & 1 ? 0x301 : 0x65)) exact = 0;
            mix(normalized[i]);
        }
        check(exact,
              "normalization workspace crosses a bank and 16-bit length");
        mix(n); tracked_realloc(a, normalized, 0); free(input);
    }
    printf("U:normalize:%08" PRIx32 "\n", digest);
    for (j = 0; j < sizeof(properties)/sizeof(properties[0]); ++j) {
        cr_init(&range, a, tracked_realloc);
        check(unicode_prop(&range, properties[j]) == 0, "Unicode property construction");
        mix(range.len); for (i = 0; i < range.len; ++i) mix(range.points[i]);
        cr_free(&range);
    }
    cr_init(&range, a, tracked_realloc);
    check(unicode_script(&range, "Cyrillic", 1) == 0 && contains(&range, 0x457) && !contains(&range, 'A'),
          "script extensions");
    cr_free(&range);
    cr_init(&range, a, tracked_realloc);
    check(unicode_general_category(&range, "Lu") == 0 && contains(&range, 0x10400UL), "32-bit category masks");
    cr_free(&range);
    cr_init(&range, a, tracked_realloc);
    check(unicode_sequence_prop("RGI_Emoji", sequence, &count, &range) == 0 && count > 1000,
          "Unicode emoji sequence properties");
    mix(count); cr_free(&range);
    printf("U:properties:%08" PRIx32 ":%" PRIu32 "\n", digest, count);
    /* The pristine reference has broken OOM cleanup in these two paths.
     * Guest-only failure injection checks the local repairs independently. */
#if CHAR_BIT == 16
    for (j = 1; j < 12; ++j) {
        a->calls = 0; a->fail_at = j;
        n = unicode_normalize(&normalized, cases[6], 2, UNICODE_NFKD, a, tracked_realloc);
        if (n >= 0) tracked_realloc(a, normalized, 0);
        check(a->live == 0 && a->blocks == 0, "normalization allocation-failure cleanup");
        a->calls = 0;
        cr_init(&range, a, tracked_realloc);
        n = unicode_script(&range, "Cyrillic", 1);
        cr_free(&range);
        check((n == 0 || n == -1) && a->live == 0 && a->blocks == 0, "script allocation-failure cleanup");
    }
    a->fail_at = 0;
#endif
}
static void regexp_case(struct Allocator *a, const char *pattern, int flags,
                        const void *text, QInt length, int wide, int expected)
{
    Octet *code, **captures;
    QInt size, n, ret, i;
    char error[128];
    code = lre_compile(&size, error, sizeof(error), pattern, strlen(pattern), flags, a);
    check(code != NULL && size > 0 && !error[0], "regexp compile");
    if (!code) { printf("DIAGNOSTIC %s\n", error); return; }
    n = lre_get_alloc_count(code);
    captures = malloc(n * sizeof(*captures));
    if (!captures) abort();
    ret = lre_exec(captures, code, text, 0, length, wide, a);
    check(ret == expected, "regexp execute");
    mix(ret); mix(lre_get_flags(code)); mix(lre_get_capture_count(code));
    for (i = 0; i < size; ++i) { if (code[i] > 255) { check(0, "bytecode octet truncation"); break; } mix(code[i]); }
    if (ret == 1) for (i = 0; i < 2*lre_get_capture_count(code); ++i)
        mix(captures[i] ? (captures[i]-(const Octet *)text)/(wide ? sizeof(uint16_t) : 1) : UINT32_MAX);
    free(captures); tracked_realloc(a, code, 0);
    check(a->live == 0 && !a->blocks, "regexp lifetime cleanup");
}
static void regexp_tests(struct Allocator *a)
{
    static const uint16_t ukrainian[] = {0x423,0x43a,0x440,0x430,0x457,0x43d,0x430};
    static const uint16_t supplementary[] = {0xd83d,0xde00,'x'};
    static const Octet latin1[] = {0xe9,0xe9};
    Octet *code, **capture;
    QInt size, ret;
    unsigned j;
    char error[128], *long_pattern, *subject;
    regexp_case(a, "^(a|ab)+b$", 0, "aabab", 5, 0, 1);
    regexp_case(a, "(?<=ab)c(?=d)", 0, "abcd", 4, 0, 1);
    regexp_case(a, "(ab)\\1", 0, "abab", 4, 0, 1);
    regexp_case(a, "(?<word>ab)\\k<word>", 0, "abab", 4, 0, 1);
    regexp_case(a, "(?i:ab)c", 0, "ABc", 3, 0, 1);
    regexp_case(a, "^a.*b$", LRE_FLAG_DOTALL, "a\nb", 3, 0, 1);
    regexp_case(a, "^b$", LRE_FLAG_MULTILINE, "a\nb\nc", 5, 0, 1);
    regexp_case(a, "^\\u00e9+$", 0, latin1, 2, 0, 1);
    regexp_case(a, "^\\p{Script=Cyrillic}+$", LRE_FLAG_UNICODE, ukrainian, 7, 1, 1);
    regexp_case(a, "^(.)x$", LRE_FLAG_UNICODE, supplementary, 3, 1, 1);
    regexp_case(a, "^(.)x$", 0, supplementary, 3, 1, 0);
    regexp_case(a, "^\\u{1f600}x$", LRE_FLAG_UNICODE, supplementary, 3, 1, 1);
    regexp_case(a, "^[\\p{ASCII}&&\\p{Letter}]+$", LRE_FLAG_UNICODE_SETS, "Az", 2, 0, 1);
    regexp_case(a, "^[\\q{ab|cd}]+$", LRE_FLAG_UNICODE_SETS, "abcdab", 6, 0, 1);
    regexp_case(a, "a(?!b)", 0, "ab", 2, 0, 0);
    printf("R:vectors:%08" PRIx32 "\n", digest);
    long_pattern = malloc(28004UL); subject = malloc(35002UL);
    if (!long_pattern || !subject) abort();
    long_pattern[0] = '^'; memset(long_pattern+1, 'a', 28000UL); long_pattern[28001]='$'; long_pattern[28002]=0;
    memset(subject, 'a', 35000UL); subject[35000] = 'b'; subject[35001] = 0;
    regexp_case(a, long_pattern, 0, subject, 28000L, 0, 1);
    regexp_case(a, "^a{35000}b$", 0, subject, 35001L, 0, 1);
    free(long_pattern); free(subject);
    printf("R:banks:%08" PRIx32 "\n", digest);
    a->reject_stack = 1;
    code = lre_compile(&size, error, sizeof(error), "(a)", 3, 0, a);
    check(!code && !size && strstr(error, "stack"), "regexp stack guard");
    a->reject_stack = 0;
    long_pattern = malloc(8194UL);
    if (!long_pattern) abort();
    for (j = 0; j < 2048; ++j) memcpy(long_pattern + 3*j, "(?:", 3);
    long_pattern[6144] = 'a'; memset(long_pattern + 6145, ')', 2048); long_pattern[8193] = 0;
    a->stack_trips = 0;
    code = lre_compile(&size, error, sizeof(error), long_pattern, 8193, 0, a);
    check(!code && !size && a->stack_trips && strstr(error, "stack"), "actual recursive stack limit");
    if (code) tracked_realloc(a, code, 0);
    free(long_pattern);
    code = lre_compile(&size, error, sizeof(error), "^(a+)+$", strlen("^(a+)+$"), 0, a);
    check(code != NULL, "interrupt fixture compile");
    if (code) {
        capture = malloc(lre_get_alloc_count(code) * sizeof(*capture));
        if (!capture) abort();
        a->timeout_after = 1; a->polls = 0;
        ret = lre_exec(capture, code, (const Octet *)"aaaaaaaaaaaaaaaaaaaab", 0, 21, 0, a);
        check(ret == LRE_RET_TIMEOUT && a->polls == 1, "regexp backtracking interrupt");
        a->timeout_after = 0;
        check(lre_exec(capture, code, (const Octet *)"aaaa", 0, 4, 0, a) == 1, "regexp recovery after interrupt");
        a->calls = 0; a->fail_at = 1;
        ret = lre_exec(capture, code, (const Octet *)"aaaaaaaaaaaaaaaaaaaab", 0, 21, 0, a);
        check(ret == LRE_RET_MEMORY_ERROR, "regexp execution allocation failure");
        a->fail_at = 0;
        free(capture); tracked_realloc(a, code, 0);
    }
    for (j = 1; j <= 24; ++j) {
        a->calls = 0; a->fail_at = j;
        code = lre_compile(&size, error, sizeof(error), "(?<word>ab|cd)+", strlen("(?<word>ab|cd)+"), 0, a);
        if (code) tracked_realloc(a, code, 0);
        check(a->live == 0 && !a->blocks, "regexp allocation-failure cleanup");
    }
    a->fail_at = 0;
    code = lre_compile(&size, error, sizeof(error), "[", 1, LRE_FLAG_UNICODE, a);
    check(!code && !size && error[0], "invalid regexp reports a diagnostic");
}
int main(int argc, char **argv)
{
    struct Allocator allocator;
    unsigned local;
    memset(&allocator, 0, sizeof(allocator));
    allocator.stack_base = linear((uintptr_t)&local);
    allocator.stack_limit = CHAR_BIT == 16 ? 8192UL : 65536UL;
    if (argc == 2 && !strcmp(argv[1], "--negative-control")) {
        check(0, "expected negative control");
        return failures ? 1 : 0;
    }
    unicode_tests(&allocator);
    regexp_tests(&allocator);
    check(allocator.live == 0 && !allocator.blocks, "all support allocations released");
    printf("QUICKJS_SUPPORT checks=%u failures=%u\n", checks, failures);
    return failures ? 1 : 0;
}
