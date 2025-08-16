#ifndef LIMITS_H
#define LIMITS_H

// Implementation limits for Ripple C
// Based on 16-bit architecture

// Char limits (assuming signed char)
#define CHAR_BIT 8
#define SCHAR_MIN (-128)
#define SCHAR_MAX 127
#define UCHAR_MAX 255

// For default char (assuming signed)
#define CHAR_MIN SCHAR_MIN
#define CHAR_MAX SCHAR_MAX

// Short limits (16-bit)
#define SHRT_MIN (-32768)
#define SHRT_MAX 32767
#define USHRT_MAX 65535

// Int limits (16-bit in our architecture)
#define INT_MIN (-32768)
#define INT_MAX 32767
#define UINT_MAX 65535

// Long limits (same as int for now)
#define LONG_MIN INT_MIN
#define LONG_MAX INT_MAX
#define ULONG_MAX UINT_MAX

// Long long limits (not supported yet, using long values)
#define LLONG_MIN LONG_MIN
#define LLONG_MAX LONG_MAX
#define ULLONG_MAX ULONG_MAX

#endif // LIMITS_H