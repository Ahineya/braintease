extern int shared;
int shared = 17;
int initialized = 19;
int zeroed;
long wide_values[2] = {0x12345678L, -2L};
static int state = 5;
static int values[3] = {1, 5, 9};
static int *focus = &values[1];
static int helper(void) { return state; }
int first_static(void) { char *s = "first"; return helper() + s[0] - 'f' + *focus - 5; }
int remote(int n) { return shared + initialized + n; }
int first_local(void) { static int counter = 30; return ++counter; }
