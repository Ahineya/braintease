/* Run through scripts/test-c-linkage.sh: these files are separate C units. */
typedef int (*callback_t)(int);
extern int shared;
extern int initialized;
extern int zeroed;
extern long wide_values[2];
extern callback_t remote_callback;
extern int remote(int);
extern int first_static(void);
extern int second_static(void);
extern int callbacks(void);
extern callback_t get_callback(void);
extern int invoke(callback_t, int);

static int state = 41;
static int helper(void) { return state; }

int globals(void) {
    if (shared != 17 || initialized != 19 || zeroed != 0) return 1;
    if (wide_values[0] != 0x12345678L || wide_values[1] != -2L) return 2;
    shared = 23;
    if (remote(1) != 43) return 3;
    if (first_static() != 5 || second_static() != 11 || helper() != 41) return 4;
    return 0;
}

int main(void) {
    int result = globals();
    callback_t fp;
    if (result) return result;
    result = callbacks();
    if (result) return result;
    fp = get_callback();
    if ((*fp)(8) != 11) return 20;
    if (invoke(remote, 2) != 44) return 21;
    if (remote_callback(3) != 45) return 22;
    return 0;
}
