/* Exercise the hosted libc path, including unequal bank spans and C16 values. */
#ifdef RCC_TEST_HOSTED_BULK
#define FUSE_X_USER 1
#endif
#include "test_string_banked_spans.c"
