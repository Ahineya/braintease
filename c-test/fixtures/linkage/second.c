typedef int (*callback_t)(int);
extern int remote(int);
static int state = 11;
static int helper(void) { return state; }
int second_static(void) { char *s = "second"; return helper() + s[0] - 's'; }
int plus_three(int n) { return n + 3; }
callback_t remote_callback = (callback_t)&remote;
callback_t table[2] = { plus_three, remote };
struct action { int number; callback_t run; };
struct action actions[2] = { {6, plus_three}, {7, remote} };
callback_t get_callback(void) { return plus_three; }
int invoke(callback_t action, int n) { return action(n); }
int callbacks(void) {
    callback_t local = plus_three;
    if (local(4) != 7) return 10;
    local = &remote;
    if ((*local)(1) != 43) return 11;
    if (table[0](5) != 8 || table[1](6) != 48) return 12;
    if (actions[0].run(actions[0].number) != 9) return 13;
    if (actions[1].run(actions[1].number) != 49) return 14;
    if (invoke(plus_three, 2) != 5) return 15;
    return 0;
}
