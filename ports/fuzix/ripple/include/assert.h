/* SPDX-License-Identifier: GPL-2.0-or-later */
/* assert may be included again after changing NDEBUG. */
#ifndef RIPPLE_ASSERT_DECLARED
#define RIPPLE_ASSERT_DECLARED
void __assert(const char *, const char *, int);
#endif
#undef assert
#ifdef NDEBUG
#define assert(expression) ((void)0)
#else
#define assert(expression) ((expression) ? (void)0 : __assert(#expression, __FILE__, __LINE__))
#endif
