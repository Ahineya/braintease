/* SPDX-License-Identifier: MIT */
#ifndef RIPPLE_INI_H
#define RIPPLE_INI_H
#include <stddef.h>
/* Ordered, bounded INI reader. Strings are borrowed until next()/free().
 * No inline comments or escapes; matching outer quotes are removed. */
typedef struct IniReader IniReader;
#define INI_SYNTAX -1
#define INI_LIMIT -2
#define INI_MEMORY -3
IniReader *ini_reader(const char *text, size_t length);
void ini_free(IniReader *reader);
int ini_next(IniReader *reader); /* 1 assignment, 0 EOF, negative error */
unsigned long ini_line(const IniReader *reader);
const char *ini_section(const IniReader *reader);
const char *ini_key(const IniReader *reader);
const char *ini_value(const IniReader *reader);
#endif
