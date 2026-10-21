static int same(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}
int main(int argc, char **argv)
{
    if (argc != 3 || !argv || !argv[0] || !argv[0][0] || argv[argc]) return 1;
    if (!same(argv[1], "hello") || !same(argv[2], "x y")) return 2;
    return 0;
}
