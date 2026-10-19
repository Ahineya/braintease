#include <netsurf_profile.h>
#include <stdio.h>
static int failures, checks;
static void check(int ok, const char *name) {
    checks++;
    if (!ok) { failures++; printf("FAIL %s\n", name); }
}
#include "talloc-check.inc"
int main(void) {
    puts("TALLOC start"); fflush(stdout);
    check_talloc();
    printf("TALLOC checks=%d failures=%d\n", checks, failures);
    return failures ? 1 : 0;
}
