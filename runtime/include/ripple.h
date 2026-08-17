#ifndef RIPPLE_H
#define RIPPLE_H

// Ripple C Runtime - Master include file
// This provides all standard library functionality available in Ripple C

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <limits.h>
#include <ctype.h>
#include <assert.h>

// Ripple-specific extensions

// Inline assembly support (compiler built-in)
// Example: __asm__("STORE A0, R0, R0");

// Version information
#define RIPPLE_VERSION_MAJOR 0
#define RIPPLE_VERSION_MINOR 1
#define RIPPLE_VERSION_PATCH 0

#endif // RIPPLE_H