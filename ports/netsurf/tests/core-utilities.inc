#include <nsutils/base64.h>
#include "utils/hashtable.h"
#include "utils/config.h"
#include <math.h>
static void check_utilities(void)
{
    char *end, *converted = NULL;
    const char *empty = " +x";
    size_t length;
    ns_octet_t source[3] = { 255, 128, 254 };
    ns_octet_t *encoded = NULL, *decoded = NULL;
    struct hash_table *hash = hash_create(17);
    ns_octet_t gzip[2] = { 31, 139 };
    char longline[520];
    const char latin[] = { 0x63, 0x61, 0x66, 0xe9, 0 };
    const char broken[] = { 0xe2, 0x82, 0 };
    uint64_t first, second;
    converted = ns_strndup("text/css; charset=utf-8", 8);
    check(converted && strcmp(converted, "text/css") == 0, "private string duplicate uses size_t ABI");
    free(converted);
    converted = ns_strndup("text/css", 0);
    check(converted && converted[0] == 0, "zero-length string duplicate");
    free(converted); converted = NULL;
    errno = 0;
    check(ns_strtoll("9223372036854775807!", &end, 10) == (long long)(UINT64_MAX >> 1) && *end == '!' && !errno, "signed 64-bit maximum");
    errno = 0;
    check(ns_strtoll("-9223372036854775808", &end, 10) == -((long long)(UINT64_MAX >> 1)) - 1 && !*end && !errno, "signed 64-bit minimum");
    errno = 0;
    check(ns_strtoll("922337203685477580800!", &end, 10) == (long long)(UINT64_MAX >> 1) && errno == ERANGE && *end == '!', "signed overflow consumes digits");
    errno = 0;
    check(ns_strtoull("18446744073709551615", &end, 10) == UINT64_MAX && !*end && !errno, "unsigned 64-bit maximum");
    errno = 0;
    check(ns_strtoull("18446744073709551616!", &end, 10) == UINT64_MAX && errno == ERANGE && *end == '!', "unsigned overflow");
    check(ns_strtoull("-1", &end, 10) == UINT64_MAX, "unsigned negative conversion");
    check(ns_strtoll("  -0x2aZ", &end, 0) == -42 && *end == 'Z', "base detection and end pointer");
    check(ns_strtoll("0778", &end, 0) == 63 && *end == '8', "octal boundary");
    check(ns_strtoll(empty, &end, 10) == 0 && end == empty, "no integer conversion");
    errno = 0;
    check(ns_strtoll("123", &end, 1) == 0 && errno == EINVAL && *end == '1', "invalid integer base");
    errno = 0;
    check(ns_strtof("1.25!", &end) == 1.25f && *end == '!' && !errno, "float conversion");
    errno = 0;
    check(isinf(ns_strtof("inf", &end)) && !errno && !*end, "explicit infinity is not overflow");
    check(ns_ceilf(220.25f) == 221.0f && ns_ceilf(-220.25f) == -220.0f,
          "float ceiling through guest double math");
    check(nsu_getmonotonic_ms(&first) == NSUERROR_OK && nsu_getmonotonic_ms(&second) == NSUERROR_OK && second >= first, "monotonic clock");
    check(nsu_base64_encode_alloc(source, 3, &encoded, &length) == NSUERROR_OK && length == 4 && memcmp(encoded, "/4D+", 4) == 0, "base64 high octets encode");
    check(nsu_base64_decode_alloc(encoded, length, &decoded, &length) == NSUERROR_OK && length == 3 && memcmp(source, decoded, 3) == 0, "base64 high octets decode");
    free(encoded); free(decoded); decoded = NULL;
    check(nsu_base64_decode_alloc((const ns_octet_t *)"", 0, &decoded, &length) == NSUERROR_OK && length == 0, "empty base64 input");
    free(decoded);
    check(hash && hash_add_inline(hash, (const ns_octet_t *)"Name:Value\nOther:Tail", 21) == NSERROR_OK && strcmp(hash_get(hash, "Other"), "Tail") == 0, "plain messages without final newline");
    check(hash_add_inline(hash, gzip, 2) == NSERROR_NOT_IMPLEMENTED, "compressed messages rejected explicitly");
    check(hash_add_inline(hash, NULL, 0) == NSERROR_OK, "empty messages");
    memset(longline, 'x', sizeof(longline)); longline[1] = ':';
    check(hash_add_inline(hash, (const ns_octet_t *)longline, sizeof(longline)) == NSERROR_BAD_SIZE, "overlength messages rejected");
    hash_destroy(hash);
    check(utf8_from_enc(latin, "ISO-8859-1", 4, &converted, &length) == NSERROR_OK && length == 5 && strcmp(converted, "café") == 0, "ParserUtils replaces iconv for Latin-1");
    free(converted); converted = NULL;
    check(utf8_to_enc("café", "ISO-8859-1", 5, &converted) == NSERROR_OK && memcmp(converted, latin, 5) == 0, "Latin-1 encoding round trip");
    free(converted); converted = NULL;
    check(utf8_to_enc(broken, "UTF-8", 2, &converted) == NSERROR_BAD_ENCODING && converted == NULL, "truncated UTF-8 rejected");
    check(utf8_to_enc("test", "UTF-16", 4, &converted) == NSERROR_BAD_ENCODING && converted == NULL, "unsupported UTF-16 conversion rejected");
    check(utf8_to_html("<&> café", "UTF-8", 0, &converted) == NSERROR_OK && strcmp(converted, "&lt;&amp;&gt; café") == 0, "HTML escaping with UTF-8");
    free(converted);
}
