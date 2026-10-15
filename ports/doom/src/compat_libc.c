/* Small private libc surface used by the Ripple Doom port.
 * Filesystem access is deliberately denied; WAD reads use w_file_port.c.
 * Integer/string formatting is real and bounded, including count-only calls.
 * Floating-point, %n, and integer types wider than long are unsupported.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdint.h>
#include <limits.h>
#include <errno.h>
#include <sys/stat.h>
#include <math.h>

struct doom_port_file { int kind; };
static FILE input_stream = {0};
static FILE output_stream = {1};
static FILE error_stream = {2};
FILE *stdin = &input_stream;
FILE *stdout = &output_stream;
FILE *stderr = &error_stream;
int errno;

static int output_stream_ok(FILE *stream) {
    return stream == stdout || stream == stderr;
}

#ifndef RCC_NATIVE
FILE *fopen(const char *path, const char *mode) { errno = ENOENT; return NULL; }
FILE *freopen(const char *path, const char *mode, FILE *stream) { errno = ENOSYS; return NULL; }
int fclose(FILE *stream) { errno = EBADF; return EOF; }
size_t fread(void *buffer, size_t size, size_t count, FILE *stream) { errno = EBADF; return 0; }
size_t fwrite(const void *buffer, size_t size, size_t count, FILE *stream) { errno = EBADF; return 0; }
int fseek(FILE *stream, long offset, int origin) { errno = EBADF; return -1; }
long ftell(FILE *stream) { errno = EBADF; return -1L; }
int feof(FILE *stream) { return 1; }
int ferror(FILE *stream) { return 1; }
int fflush(FILE *stream) {
    if (stream == NULL || output_stream_ok(stream)) return 0;
    errno = EBADF; return EOF;
}
int fgetc(FILE *stream) { errno = EBADF; return EOF; }
int getc(FILE *stream) { return fgetc(stream); }
char *fgets(char *buffer, int size, FILE *stream) { errno = EBADF; return NULL; }
int fputc(int ch, FILE *stream) {
    if (!output_stream_ok(stream)) { errno = EBADF; return EOF; }
    putchar(ch);
    return (unsigned char)ch;
}
int putc(int ch, FILE *stream) { return fputc(ch, stream); }
int fputs(const char *text, FILE *stream) {
    if (!output_stream_ok(stream)) { errno = EBADF; return EOF; }
    while (*text) putchar(*text++);
    return 0;
}
int remove(const char *path) { errno = ENOSYS; return -1; }
int rename(const char *old_path, const char *new_path) { errno = ENOSYS; return -1; }
#endif
int mkdir(const char *path, mode_t mode) { errno = ENOSYS; return -1; }
int stat(const char *path, struct stat *info) { errno = ENOENT; return -1; }

typedef struct {
    char *buffer; /* Next writable cell, including the reserved terminator. */
    size_t capacity; /* Remaining cells; used counts even truncated output. */
    size_t used;
    int console;
} FormatOutput;

static void output_char(FormatOutput *out, char ch) {
    if (out->console) putchar(ch);
    if (out->buffer && out->capacity > 1UL) {
        *out->buffer++ = ch;
        --out->capacity;
    }
    ++out->used;
}

static void output_repeat(FormatOutput *out, char ch, size_t count) {
    if (!count) return;
    out->used += count;
    if (out->console) {
        while (count) { putchar(ch); --count; }
    } else if (out->buffer && out->capacity > 1UL) {
        char *destination = out->buffer;
        if (count >= out->capacity) count = out->capacity - 1UL;
        out->capacity -= count;
        while (count) { *destination++ = ch; --count; }
        out->buffer = destination;
    }
}

static void output_text(FormatOutput *out, const char *text, size_t count) {
    if (!count) return;
    out->used += count;
    if (out->console) {
        while (count) { putchar(*text++); --count; }
    } else if (out->buffer && out->capacity > 1UL) {
        char *destination = out->buffer;
        if (count >= out->capacity) count = out->capacity - 1UL;
        out->capacity -= count;
        while (count) { *destination++ = *text++; --count; }
        out->buffer = destination;
    }
}

static int output_finish(FormatOutput *out, int error) {
    if (out->buffer && out->capacity) *out->buffer = 0;
    if (error) { errno = error; return -1; }
    if (out->used > INT_MAX) { errno = EOVERFLOW; return -1; }
    return (int)out->used;
}

static int format_output(FormatOutput *out, const char *format, va_list args) {
    while (*format) {
        /* Literal runs need neither conversion state nor argument decoding. */
        if (*format != '%') {
            const char *text = format;
            size_t count = 0;
            do { ++count; ++format; }
            while (*format && *format != '%');
            output_text(out, text, count);
            continue;
        }
        int left = 0;
        int plus = 0;
        int space = 0;
        int alternate = 0;
        int zero = 0;
        int width = 0;
        int precision = -1;
        int length = 0; /* 0=int, 1=long, 2=size_t, 3=short, 4=char */
        char conversion;
        ++format;
        for (;;) {
            if (*format == '-') left = 1;
            else if (*format == '+') plus = 1;
            else if (*format == ' ') space = 1;
            else if (*format == '#') alternate = 1;
            else if (*format == '0') zero = 1;
            else break;
            ++format;
        }
        if (*format == '*') {
            width = va_arg(args, int);
            ++format;
            if (width == INT_MIN) return output_finish(out, EOVERFLOW);
            if (width < 0) { left = 1; width = -width; }
        } else {
            while (*format >= '0' && *format <= '9') {
                int digit = *format++ - '0';
                if (width > (INT_MAX - digit) / 10) return output_finish(out, EOVERFLOW);
                width = width * 10 + digit;
            }
        }
        if (*format == '.') {
            ++format;
            precision = 0;
            if (*format == '*') { precision = va_arg(args, int); ++format; }
            else while (*format >= '0' && *format <= '9') {
                int digit = *format++ - '0';
                if (precision > (INT_MAX - digit) / 10) return output_finish(out, EOVERFLOW);
                precision = precision * 10 + digit;
            }
        }
        if (*format == 'l') {
            length = 1; ++format;
            if (*format == 'l') return output_finish(out, EINVAL);
        } else if (*format == 'z') { length = 2; ++format; }
        else if (*format == 'h') {
            length = 3; ++format;
            if (*format == 'h') { length = 4; ++format; }
        }
        conversion = *format;
        if (!conversion) return output_finish(out, EINVAL);
        ++format;
        if (conversion == 's' || conversion == 'c' || conversion == '%') {
            const char *text;
            char one;
            size_t count = 0;
            size_t padding;
            if (length) return output_finish(out, EINVAL);
            if (conversion == 's') {
                text = va_arg(args, const char *);
                if (!text) text = "(null)";
                while ((precision < 0 || count < (size_t)precision) && text[count]) ++count;
            } else {
                one = conversion == '%' ? '%' : (char)va_arg(args, int);
                text = &one;
                count = 1;
            }
            padding = (size_t)width > count ? (size_t)width - count : 0;
            if (!left) output_repeat(out, ' ', padding);
            output_text(out, text, count);
            if (left) output_repeat(out, ' ', padding);
        } else if (conversion == 'd' || conversion == 'i' || conversion == 'u' ||
                   conversion == 'o' || conversion == 'x' || conversion == 'X' || conversion == 'p') {
            unsigned long value;
            unsigned long original;
            unsigned long base = conversion == 'o' ? 8UL :
                (conversion == 'x' || conversion == 'X' || conversion == 'p') ? 16UL : 10UL;
            char digits[34];
            char sign = 0;
            char prefix[2];
            int prefix_count = 0;
            int digit_count = 0;
            size_t zeroes;
            size_t total;
            size_t padding;
            int i;
            if (conversion == 'p') {
                if (length) return output_finish(out, EINVAL);
                value = (unsigned long)(uintptr_t)va_arg(args, void *);
                prefix[0] = '0'; prefix[1] = 'x'; prefix_count = 2;
            } else if (conversion == 'd' || conversion == 'i') {
                long signed_value;
                if (length == 1) signed_value = va_arg(args, long);
                else if (length == 2) signed_value = (long)va_arg(args, ptrdiff_t);
                else {
                    int number = va_arg(args, int);
                    if (length == 3) number = (short)number;
                    if (length == 4) number = (signed char)number;
                    signed_value = number;
                }
                if (signed_value < 0) { sign = '-'; value = 0UL - (unsigned long)signed_value; }
                else { value = (unsigned long)signed_value; sign = plus ? '+' : space ? ' ' : 0; }
            } else {
                if (length == 1) value = va_arg(args, unsigned long);
                else if (length == 2) value = (unsigned long)va_arg(args, size_t);
                else {
                    unsigned int number = va_arg(args, unsigned int);
                    if (length == 3) number = (unsigned short)number;
                    if (length == 4) number = (unsigned char)number;
                    value = number;
                }
            }
            original = value;
            if (value || precision != 0) {
                do {
                    unsigned int digit = (unsigned int)(value % base);
                    digits[digit_count++] = (char)(digit < 10 ? '0' + digit :
                        (conversion == 'X' ? 'A' : 'a') + digit - 10);
                    value /= base;
                } while (value);
            }
            if (alternate && base == 16UL && original && conversion != 'p') {
                prefix[0] = '0'; prefix[1] = conversion == 'X' ? 'X' : 'x'; prefix_count = 2;
            }
            zeroes = precision > digit_count ? (size_t)(precision - digit_count) : 0;
            if (alternate && base == 8UL && !zeroes && (!digit_count || digits[digit_count - 1] != '0'))
                zeroes = 1;
            total = (size_t)digit_count + zeroes + prefix_count + (sign ? 1 : 0);
            padding = (size_t)width > total ? (size_t)width - total : 0;
            if (!left && (!zero || precision >= 0)) output_repeat(out, ' ', padding);
            if (sign) output_char(out, sign);
            for (i = 0; i < prefix_count; ++i) output_char(out, prefix[i]);
            if (!left && zero && precision < 0) output_repeat(out, '0', padding);
            output_repeat(out, '0', zeroes);
            while (digit_count) output_char(out, digits[--digit_count]);
            if (left) output_repeat(out, ' ', padding);
        } else return output_finish(out, EINVAL);
    }
    return output_finish(out, 0);
}

int vsnprintf(char *buffer, size_t size, const char *format, va_list args) {
    FormatOutput out;
    if ((!buffer && size) || !format) { errno = EINVAL; return -1; }
    out.buffer = buffer; out.capacity = size; out.used = 0; out.console = 0;
    return format_output(&out, format, args);
}
int snprintf(char *buffer, size_t size, const char *format, ...) {
    va_list args;
    int result;
    va_start(args, format);
    result = vsnprintf(buffer, size, format, args);
    va_end(args);
    return result;
}
int vsprintf(char *buffer, const char *format, va_list args) {
    return vsnprintf(buffer, SIZE_MAX, format, args);
}
int sprintf(char *buffer, const char *format, ...) {
    va_list args;
    int result;
    va_start(args, format);
    result = vsprintf(buffer, format, args);
    va_end(args);
    return result;
}
int vfprintf(FILE *stream, const char *format, va_list args) {
    FormatOutput out;
    if (!output_stream_ok(stream)) { errno = EBADF; return -1; }
    if (!format) { errno = EINVAL; return -1; }
    out.buffer = NULL; out.capacity = 0; out.used = 0; out.console = 1;
    return format_output(&out, format, args);
}
int fprintf(FILE *stream, const char *format, ...) {
    va_list args;
    int result;
    va_start(args, format);
    result = vfprintf(stream, format, args);
    va_end(args);
    return result;
}
int vprintf(const char *format, va_list args) { return vfprintf(stdout, format, args); }

static int lower_ascii(int ch) { return ch >= 'A' && ch <= 'Z' ? ch + ('a' - 'A') : ch; }
int strncasecmp(const char *left, const char *right, size_t count) {
    while (count) {
        int a = lower_ascii((unsigned char)*left++);
        int b = lower_ascii((unsigned char)*right++);
        if (a != b) return a < b ? -1 : 1;
        if (!a) return 0;
        --count;
    }
    return 0;
}
int strcasecmp(const char *left, const char *right) { return strncasecmp(left, right, SIZE_MAX); }
char *strdup(const char *text) {
    size_t size = strlen(text) + 1UL;
    char *result = malloc(size);
    if (result) memcpy(result, text, size);
    return result;
}
char *strstr(const char *text, const char *needle) {
    size_t length = strlen(needle);
    if (!length) return (char *)text;
    while (*text) {
        if (strncmp(text, needle, length) == 0) return (char *)text;
        ++text;
    }
    return NULL;
}
char *strerror(int error) {
    if (error == ENOENT) return "file access unavailable";
    if (error == EBADF) return "invalid stream";
    if (error == ERANGE || error == EOVERFLOW) return "numeric range exceeded";
    if (error == EINVAL) return "invalid argument or unsupported format";
    if (error == ENOSYS) return "operation unavailable";
    return "port library error";
}
char *getenv(const char *name) { return NULL; }

static int number_digit(int ch) {
    if (ch >= '0' && ch <= '9') return ch - '0';
    ch = lower_ascii(ch);
    if (ch >= 'a' && ch <= 'z') return ch - 'a' + 10;
    return -1;
}
static unsigned long parse_number(const char *text, char **end, int base, int is_signed, int *negative) {
    const char *start = text;
    const char *digits;
    unsigned long result = 0;
    unsigned long limit;
    int overflow = 0;
    *negative = 0;
    if (end) *end = (char *)start;
    if (base != 0 && (base < 2 || base > 36)) { errno = EINVAL; return 0; }
    while (*text == ' ' || *text == '\t' || *text == '\n' || *text == '\r' || *text == '\f' || *text == '\v') ++text;
    if (*text == '+' || *text == '-') { *negative = *text == '-'; ++text; }
    if ((base == 0 || base == 16) && text[0] == '0' && (text[1] == 'x' || text[1] == 'X') &&
        number_digit(text[2]) >= 0 && number_digit(text[2]) < 16) { base = 16; text += 2; }
    if (!base) base = *text == '0' ? 8 : 10;
    limit = is_signed ? (*negative ? (unsigned long)LONG_MAX + 1UL : (unsigned long)LONG_MAX) : ULONG_MAX;
    digits = text;
    while (*text) {
        int digit = number_digit(*text);
        if (digit < 0 || digit >= base) break;
        if (result > (limit - (unsigned long)digit) / (unsigned long)base) overflow = 1;
        else if (!overflow) result = result * (unsigned long)base + (unsigned long)digit;
        ++text;
    }
    if (text == digits) return 0;
    if (end) *end = (char *)text;
    if (overflow) { errno = ERANGE; return limit; }
    return result;
}
long strtol(const char *text, char **end, int base) {
    int negative;
    unsigned long value = parse_number(text, end, base, 1, &negative);
    return (long)(negative ? 0UL - value : value);
}
unsigned long strtoul(const char *text, char **end, int base) {
    int negative;
    unsigned long value;
    int saved_errno = errno;
    errno = 0;
    value = parse_number(text, end, base, 0, &negative);
    if (errno == ERANGE) return ULONG_MAX;
    if (!errno) errno = saved_errno;
    return negative ? 0UL - value : value;
}
long atol(const char *text) { return strtol(text, NULL, 10); }
int atoi(const char *text) { return (int)strtol(text, NULL, 10); }

double fabs(double value) {
    union { double real; uint64_t bits; } number;
    number.real = value;
    number.bits &= UINT64_C(0x7fffffffffffffff);
    return number.real;
}

#ifdef RCC_NATIVE
int printf(const char *format, ...) {
    va_list args;
    int result;
    va_start(args, format);
    result = vprintf(format, args);
    va_end(args);
    return result;
}
#endif
