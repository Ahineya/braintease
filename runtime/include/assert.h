#ifndef ASSERT_H
#define ASSERT_H

void putchar(int c);
void abort(void);

#ifdef NDEBUG
    #define assert(expr) ((void)0)
#else
    #define assert(expr) \
        do { \
            if (!(expr)) { \
                putchar('A'); \
                putchar('S'); \
                putchar('S'); \
                putchar('E'); \
                putchar('R'); \
                putchar('T'); \
                putchar('!'); \
                putchar('\n'); \
                abort(); \
            } \
        } while (0)
#endif

#define static_assert(expr, msg) _Static_assert(expr, msg)

#endif /* ASSERT_H */
