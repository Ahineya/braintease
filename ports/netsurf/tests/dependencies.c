/* Runs inside both real guests; the same source also runs as a host oracle. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <netsurf_c16.h>
#include <libwapcaplet/libwapcaplet.h>
#include <parserutils/charset/codec.h>
#include <parserutils/charset/utf8.h>
#include <hubbub/parser.h>
#include <libcss/fpmath.h>
#ifdef RIPPLE_MMU
#include <unistd.h>
#endif

static unsigned checks, failures;
#define CHECK(test) do { checks++; if (!(test)) { failures++; printf("FAIL %u %s:%u: %s\n", checks, __FILE__, (unsigned)__LINE__, #test); } } while (0)

static void strings(void)
{
    lwc_string *a = NULL, *b = NULL, *lower = NULL;
    bool equal = false;
    CHECK(lwc_intern_string("NetSurf", 7, &a) == lwc_error_ok);
    CHECK(lwc_intern_string("NetSurf", 7, &b) == lwc_error_ok && a == b);
    CHECK(lwc_string_length(a) == 7 && !strcmp(lwc_string_data(a), "NetSurf"));
    CHECK(lwc_string_tolower(a, &lower) == lwc_error_ok);
    CHECK(!strcmp(lwc_string_data(lower), "netsurf"));
    CHECK(lwc_string_caseless_isequal(a, lower, &equal) == lwc_error_ok && equal);
    lwc_string_unref(b); lwc_string_unref(a); lwc_string_unref(lower);
    {
        char *large = malloc(70001UL);
        lwc_string *whole = NULL, *part = NULL;
        CHECK(large != NULL);
        if (!large) return;
        memset(large, 'x', 70000UL); large[70000UL] = 0;
        large[63999UL] = 'b'; large[64000UL] = 'c';
        CHECK(lwc_intern_string(large, 70000UL, &whole) == lwc_error_ok);
        if (whole) {
            CHECK(lwc_string_length(whole) == 70000UL && !memcmp(lwc_string_data(whole), large, 70000UL));
            CHECK(lwc_intern_substring(whole, 63998UL, 4, &part) == lwc_error_ok);
            if (part) { CHECK(!strcmp(lwc_string_data(part), "xbcx")); lwc_string_unref(part); }
            lwc_string_unref(whole);
        }
        free(large);
    }
}

static void codecs(void)
{
    static const ns_octet_t text[] = {'A', 0xd0, 0x96, 0xf0, 0x9f, 0x99, 0x82};
    parserutils_charset_codec *codec = NULL;
    ns_octet_t output[32], encoded[16];
    const ns_octet_t *source = text;
    ns_octet_t *dest = output;
    size_t inlen = sizeof(text), outlen = sizeof(output);
    size_t used, clen;
    uint32_t scalar;
    CHECK(parserutils_charset_codec_create("UTF-8", &codec) == PARSERUTILS_OK);
    CHECK(parserutils_charset_codec_decode(codec, &source, &inlen, &dest, &outlen) == PARSERUTILS_OK);
    used = sizeof(output) - outlen;
    CHECK(inlen == 0 && used == 12 && ns_read_be32(output) == 'A');
    CHECK(ns_read_be32(output + 4) == 0x416 && ns_read_be32(output + 8) == 0x1f642UL);
    source = output; inlen = used; dest = encoded; outlen = sizeof(encoded);
    CHECK(parserutils_charset_codec_encode(codec, &source, &inlen, &dest, &outlen) == PARSERUTILS_OK);
    CHECK(inlen == 0 && sizeof(encoded) - outlen == sizeof(text) && !memcmp(encoded, text, sizeof(text)));
    /* A sequence split across reads, followed by an intentionally tiny output. */
    CHECK(parserutils_charset_codec_reset(codec) == PARSERUTILS_OK);
    source = text + 3; inlen = 2; dest = output; outlen = sizeof(output);
    CHECK(parserutils_charset_codec_decode(codec, &source, &inlen, &dest, &outlen) == PARSERUTILS_OK && inlen == 0);
    source = text + 5; inlen = 2; outlen = 0;
    CHECK(parserutils_charset_codec_decode(codec, &source, &inlen, &dest, &outlen) == PARSERUTILS_NOMEM && inlen == 0);
    source = text; inlen = 0; dest = output; outlen = 4;
    CHECK(parserutils_charset_codec_decode(codec, &source, &inlen, &dest, &outlen) == PARSERUTILS_OK);
    CHECK(outlen == 0 && ns_read_be32(output) == 0x1f642UL);
    source = output; inlen = 3; dest = encoded; outlen = sizeof(encoded);
    CHECK(parserutils_charset_codec_encode(codec, &source, &inlen, &dest, &outlen) == PARSERUTILS_NEEDDATA && inlen == 3);
    CHECK(parserutils_charset_codec_destroy(codec) == PARSERUTILS_OK);
    CHECK(parserutils_charset_utf8_to_ucs4(text + 1, 2, &scalar, &clen) == PARSERUTILS_OK && scalar == 0x416 && clen == 2);
    CHECK(parserutils_charset_codec_create("windows-1252", &codec) == PARSERUTILS_OK);
    encoded[0] = 0x80; source = encoded; inlen = 1; dest = output; outlen = sizeof(output);
    CHECK(parserutils_charset_codec_decode(codec, &source, &inlen, &dest, &outlen) == PARSERUTILS_OK && ns_read_be32(output) == 0x20ac);
    CHECK(parserutils_charset_codec_destroy(codec) == PARSERUTILS_OK);
    CHECK(parserutils_charset_codec_create("UTF-16", &codec) == PARSERUTILS_BADENCODING);
}

struct tokens { unsigned tags, ends, eof; size_t length; ns_octet_t text[128]; };
static hubbub_error token(const hubbub_token *value, void *context)
{
    struct tokens *result = context;
    if (value->type == HUBBUB_TOKEN_START_TAG) result->tags++;
    if (value->type == HUBBUB_TOKEN_END_TAG) result->ends++;
    if (value->type == HUBBUB_TOKEN_EOF) result->eof++;
    if (value->type == HUBBUB_TOKEN_CHARACTER) {
        if (value->data.character.len > sizeof(result->text) - result->length) return HUBBUB_NOMEM;
        memcpy(result->text + result->length, value->data.character.ptr, value->data.character.len);
        result->length += value->data.character.len;
    }
    return HUBBUB_OK;
}

static void html(void)
{
    /* Literal UTF-8 also avoids RCC's separately recorded numeric-string
     * escape defect (test_string_hex_octets), without weakening this test. */
    static const char document[] = "<!doctype html><h1>NetSurf &amp; C16</h1><p>Ж &#x1f642;</p>";
    static const char expected[] = "NetSurf & C16Ж 🙂";
    hubbub_parser *parser = NULL;
    hubbub_parser_optparams option;
    struct tokens result;
    size_t i;
    memset(&result, 0, sizeof(result));
    CHECK(hubbub_parser_create("UTF-8", true, &parser) == HUBBUB_OK);
    option.token_handler.handler = token; option.token_handler.pw = &result;
    CHECK(hubbub_parser_setopt(parser, HUBBUB_PARSER_TOKEN_HANDLER, &option) == HUBBUB_OK);
    /* Every tag, entity and UTF-8 sequence crosses chunk boundaries. */
    for (i = 0; i < sizeof(document) - 1; i++)
        if (hubbub_parser_parse_chunk(parser, (const ns_octet_t *)document + i, 1) != HUBBUB_OK) break;
    CHECK(i == sizeof(document) - 1);
    CHECK(hubbub_parser_completed(parser) == HUBBUB_OK);
    CHECK(result.tags == 2 && result.ends == 2 && result.eof == 1);
    CHECK(result.length == sizeof(expected) - 1 && !memcmp(result.text, expected, sizeof(expected) - 1));
    if (result.length != sizeof(expected) - 1 || memcmp(result.text, expected, sizeof(expected) - 1)) {
        printf("HTML text (%lu):", (unsigned long)result.length);
        for (i = 0; i < result.length; i++) printf(" %x", (unsigned)result.text[i]);
        putchar('\n');
    }
    CHECK(hubbub_parser_destroy(parser) == HUBBUB_OK);
}

static void fixed(void)
{
    CHECK(INTTOFIX(96) == 98304L);
    CHECK(INTTOFIX(640) == 655360L);
    CHECK(INTTOFIX(40000L) == 40960000L);
    CHECK(FDIV(INTTOFIX(640), INTTOFIX(2)) == INTTOFIX(320));
    CHECK(FMUL(INTTOFIX(96), INTTOFIX(2)) == INTTOFIX(192));
    CHECK(FADD(INT32_MAX, 1) == INT32_MAX);
    CHECK(FSUB(INT32_MIN, 1) == INT32_MIN);
    CHECK(FADD(INT32_MIN, -1) == INT32_MIN);
    CHECK(FSUB(INT32_MAX, -1) == INT32_MAX);
    CHECK(INTTOFIX(INT32_MAX) == INT32_MAX);
    CHECK(INTTOFIX(INT32_MIN) == INT32_MIN);
}

#include "css.c"
#include "dom.c"

int main(void)
{
#ifdef RIPPLE_MMU
    uintptr_t initial_break = (uintptr_t)sbrk(0), final_break;
#endif
    strings(); codecs(); html(); fixed(); styles(); dom_tests();
#ifdef RIPPLE_MMU
    final_break = (uintptr_t)sbrk(0);
    if (initial_break != UINT32_MAX && final_break != UINT32_MAX) {
        /* The guest encodes bank:offset in pointers. Gaps between 64000-cell
         * banks are not allocated cells. brk growth measures reserved heap,
         * including allocator slack, not the peak live allocation total. */
        unsigned long first = (initial_break >> 16) * 64000UL + (initial_break & 65535UL);
        unsigned long last = (final_break >> 16) * 64000UL + (final_break & 65535UL);
        printf("NETSURF_HEAP growth_cells=%lu\n", last - first);
    } else {
        puts("FAIL cannot measure guest heap break"); failures++;
    }
#endif
    printf("NETSURF_%s %u checks, %u failures\n", failures ? "FAILED" : "OK", checks, failures);
    return failures ? 1 : 0;
}
