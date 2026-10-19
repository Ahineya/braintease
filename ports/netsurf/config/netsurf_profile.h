#ifndef NS_C16_PROFILE_H
#define NS_C16_PROFILE_H
#include <limits.h>
#include <netsurf_c16.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#if CHAR_BIT == 16
/* Optimization/diagnostic-only GNU suffix attributes; see the recorded
 * test_function_suffix_attribute compatibility regression. */
#define __attribute__(attributes)
#define PRId32 "ld"
#define PRIu32 "lu"
#define PRIx32 "lx"
#define PRId64 "lld"
#define PRIu64 "llu"
#define PRIx64 "llx"
#define PRIxPTR "lx"
#define PRIuPTR "lu"
#define SCNx32 "lx"
#define PRIX32 "lX"
#else
#include <inttypes.h>
#endif
#define NETSURF_LOG_LEVEL CRITICAL
#define NETSURF_BUILTIN_LOG_FILTER "level:CRITICAL"
#define NETSURF_BUILTIN_VERBOSE_FILTER "level:CRITICAL"
#define NETSURF_UA_FORMAT_STRING "NetSurf (%s) %d.%d"
#define NETSURF_HOMEPAGE "about:blank"
long long ns_strtoll(const char *, char **, int);
unsigned long long ns_strtoull(const char *, char **, int);
float ns_strtof(const char *, char **);
float ns_ceilf(float);
#endif
