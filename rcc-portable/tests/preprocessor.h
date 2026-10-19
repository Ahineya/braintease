#ifndef PORTABLE_PREPROCESSOR_FIXTURE_H
#define PORTABLE_PREPROCESSOR_FIXTURE_H
#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>
#define PP_JOIN_RAW(a,b) a##b
#define PP_JOIN(a,b) PP_JOIN_RAW(a,b)
#define PP_TEXT_RAW(x) #x
#define PP_TEXT(x) PP_TEXT_RAW(x)
#define PP_PLUS(x) ((x)+1)
#define PP_APPLY(f,...) f(__VA_ARGS__)
#define PP_PREFIX portable_
#if !defined(__RCC_PORTABLE__) || __STDC_VERSION__ < 201112L
#error missing portable compiler definitions
#endif
#if UINT64_MAX != 18446744073709551615ULL || (-1 < 1U)
#error preprocessor integers must use intmax_t semantics
#endif
#if '\xffff' != -1 || '\x8000' != -32768
#error preprocessor character constants must respect signed C16 characters
#endif
#if (1 || 1/0) && (0 ? 1/0 : 1)
#define PP_CHOICE 7
#else
#error conditional evaluation failed
#endif
#if 0
#include "nonexistent.h"
#define FOREIGN(a b) broken syntax
#elif PP_CHOICE == 7
#define PP_MAGIC UINT32_C(70000)
#else
#error wrong elif branch
#endif
struct PP_JOIN(PP_PREFIX,pair) {int tag;uint32_t value;};
#endif
