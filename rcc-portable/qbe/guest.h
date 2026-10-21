/* C16 host support; this is not part of the generated application's ABI. */
#undef assert
#define assert(x) ((x) ? (void)0 : qbe_assert(#x, __FILE__, __LINE__))
void qbe_assert(const char *, const char *, long);
int qbe_scanfloat(FILE *, void *, int);
void qbe_qsort(void *, size_t, size_t, long (*)(const void *, const void *));
void qbe_error(char *, ...);
void qbe_output_ready(void);
void qbe_scratch_exhausted(void);
#define err(...)                                                                                   \
    do {                                                                                           \
        qbe_error(__VA_ARGS__);                                                                    \
        for (;;) {                                                                                 \
        }                                                                                          \
    } while (0)
