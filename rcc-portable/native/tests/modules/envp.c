int main(int argc, char **argv, char **envp)
{
    unsigned i;
    if (argc != 2 || !argv || !argv[0] || !argv[1] || argv[2] || !envp) return 1;
    if (argv[1][0] != 'c' || argv[1][1] != 'h') return 2;
    for (i = 0; i < 128; i++)
        if (!envp[i]) return 0;
    return 3;
}
