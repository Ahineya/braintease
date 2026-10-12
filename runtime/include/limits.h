#ifndef LIMITS_H
#define LIMITS_H

// Implementation limits for Ripple C
// Based on 16-bit architecture

// A C byte is one addressable 16-bit VM cell. Plain char is signed.
#define CHAR_BIT 16
#define SCHAR_MIN (-32767-1)
#define SCHAR_MAX 32767
#define UCHAR_MAX 65535U

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

// Long limits (32-bit)
#define LONG_MIN (-2147483647L-1)
#define LONG_MAX 2147483647L
#define ULONG_MAX 4294967295UL

// Long long limits (64-bit)
#define LLONG_MIN (-9223372036854775807LL-1)
#define LLONG_MAX 9223372036854775807LL
#define ULLONG_MAX 18446744073709551615ULL

#endif // LIMITS_H