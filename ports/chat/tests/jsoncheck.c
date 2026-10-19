#include "../json.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>
static void good(const char *s) {
    CJ j;
    assert(cj_parse(&j, s, strlen(s)));
    cj_free(&j);
}
static void bad(const char *s) {
    CJ j;
    assert(!cj_parse(&j, s, strlen(s)));
}
int main(void) {
    CJ j;
    char *s;
    unsigned long n;
    good("{\"a\":[1,null,true,false,{},[]],\"b\":-2.3e+4}");
    bad("{\"a\":1,\"\\u0061\":2}");
    bad("[1,]");
    bad("{\"a\":1,}");
    bad("01");
    bad("truefalse");
    bad("\"\\ud800\"");
    bad("\"\\udc00\"");
    bad("\"\xc0\x80\"");
    assert(!cj_parse(&j, "null\0", 5));
    assert(!cj_parse(&j, "\"\\\0\"", 4));
    assert(cj_parse(&j, "{\"text\":\"\\ud83d\\ude42Ж\",\"n\":4294967295}",
                    strlen("{\"text\":\"\\ud83d\\ude42Ж\",\"n\":4294967295}")));
    s = cj_string(&j, cj_field(&j, 0, "text"));
    assert(s && !strcmp(s, "🙂Ж"));
    free(s);
    assert(cj_number(&j, cj_field(&j, 0, "n"), &n) && n == 4294967295UL);
    cj_free(&j);
    puts("Strict JSON and UTF-8 checks passed");
    return 0;
}
