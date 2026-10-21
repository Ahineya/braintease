/* Native x86 guest pipeline with self-contained ELF modules. */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#define MAX_ARGS 256

static char *temporary[MAX_ARGS * 4], *options[MAX_ARGS], *directory;
static char *inputs[MAX_ARGS], *objects[MAX_ARGS];
static const char *tools, *sdk;
static int option_count, verbose, temporary_count, input_count, object_count;

static void cleanup(void)
{
    int i;
    for (i = 0; i < temporary_count; i++)
        if (temporary[i])
            unlink(temporary[i]);
    if (directory)
        rmdir(directory);
}

static void die(const char *text)
{
    fprintf(stderr, "rcc-native-cc: %s\n", text);
    exit(1);
}

static char *path(const char *base, const char *name)
{
    char *p = malloc(strlen(base) + strlen(name) + 2);
    if (!p)
        die("out of memory");
    sprintf(p, "%s/%s", base, name);
    return p;
}

static void append(char **argv, int *n, char *arg)
{
    if (*n >= MAX_ARGS - 1)
        die("too many arguments");
    argv[(*n)++] = arg;
    argv[*n] = NULL;
}

static void run(char **argv)
{
    const char *program = tools ? path(tools, argv[0]) : argv[0];
    pid_t child;
    int status, i;
    if (verbose) {
        for (i = 0; argv[i]; i++)
            fprintf(stderr, "%s%s", i ? " " : "", argv[i]);
        fputc('\n', stderr);
    }
    fflush(stdout);
    fflush(stderr);
    child = fork();
    if (child < 0)
        die("cannot start compiler stage");
    if (!child) {
        execvp(program, argv);
        fprintf(stderr, "rcc-native-cc: cannot execute %s\n", program);
        _exit(127);
    }
    while (waitpid(child, &status, 0) < 0)
        if (errno != EINTR)
            die("cannot wait for compiler stage");
    if (!WIFEXITED(status))
        die("compiler stage terminated by signal");
    if (WEXITSTATUS(status))
        exit(WEXITSTATUS(status));
}

static char *temporary_path(const char *suffix)
{
    char name[40], *p;
    if (temporary_count >= MAX_ARGS * 4)
        die("too many intermediates");
    sprintf(name, "unit%u.%s", (unsigned)temporary_count, suffix);
    p = path(directory, name);
    temporary[temporary_count++] = p;
    return p;
}

static char *compile(char *source, char *output, int stage)
{
    char *command[MAX_ARGS], *ir = temporary_path("ir"), *ssa = temporary_path("ssa");
    char *assembly = temporary_path("s"), *object = stage == 4 ? output : temporary_path("o");
    int i, n = 0;
    append(command, &n, "rcc-c");
    append(command, &n, "--target");
    append(command, &n, "native");
    if (sdk) {
        append(command, &n, "-DRIPPLE_MMU=1");
        append(command, &n, "-DFUSE_X_USER=1");
        append(command, &n, "-DFUSE_X_BOUNDED_HEAP=1");
    }
    for (i = 0; i < option_count; i++)
        append(command, &n, options[i]);
    if (sdk) {
        /* Explicit -I paths override the SDK's fallback headers, including
         * compiler-private declarations and buffered stdio wrappers. */
        append(command, &n, "-I");
        append(command, &n, path(sdk, "include"));
    }
    if (!stage)
        append(command, &n, "-E");
    append(command, &n, source);
    append(command, &n, "-o");
    append(command, &n, stage <= 1 ? output : ir);
    run(command);
    if (stage <= 1)
        return NULL;
    n = 0;
    append(command, &n, "rcc-qbe");
    append(command, &n, "--relocatable");
    if (sdk)
        append(command, &n, "--no-start");
    append(command, &n, ir);
    append(command, &n, "-o");
    append(command, &n, stage == 2 ? output : ssa);
    run(command);
    if (stage == 2)
        return NULL;
    n = 0;
    append(command, &n, "qbe");
    append(command, &n, "-t");
    append(command, &n, "amd64_sysv");
    append(command, &n, "-o");
    append(command, &n, stage == 3 ? output : assembly);
    append(command, &n, ssa);
    run(command);
    if (stage == 3)
        return NULL;
    n = 0;
    append(command, &n, "rcc-x64-as");
    append(command, &n, assembly);
    append(command, &n, "-o");
    append(command, &n, object);
    run(command);
    return object;
}

int main(int argc, char **argv)
{
    char *output = NULL, *command[MAX_ARGS];
    int i, n = 0, stage = 5;
    tools = getenv("RCC_NATIVE_TOOLS");
    sdk = getenv("RCC_NATIVE_SDK");
    if (sdk && !*sdk)
        sdk = NULL;
    if (tools && !*tools)
        tools = NULL;
    for (i = 1; i < argc; i++) {
        char *arg = argv[i];
        if (!strcmp(arg, "-o") && i + 1 < argc && !output)
            output = argv[++i];
        else if (!strcmp(arg, "--tools") && i + 1 < argc)
            tools = argv[++i];
        else if (!strcmp(arg, "--sdk") && i + 1 < argc)
            sdk = argv[++i];
        else if (!strcmp(arg, "-v"))
            verbose = 1;
        else if (!strcmp(arg, "-E"))
            stage = 0;
        else if (!strcmp(arg, "--emit-ir"))
            stage = 1;
        else if (!strcmp(arg, "--emit-qbe"))
            stage = 2;
        else if (!strcmp(arg, "-S"))
            stage = 3;
        else if (!strcmp(arg, "-c"))
            stage = 4;
        else if (!strcmp(arg, "--help")) {
            puts("Usage: rcc-native-cc SOURCE.c/INPUT.o/LIBRARY.a ... [-o PROGRAM] [-I/-D/-U ...] "
                 "[-v]");
            puts("Stops (one source): -E, --emit-ir, --emit-qbe, -S, -c. Target: C16 x86_64 "
                 "Riplix.");
            puts("--sdk DIR (or RCC_NATIVE_SDK): C16 headers, libc.a and crt0-N.o startup.");
            puts("Experimental: C16 scalar/variadic/aggregate calls and globals; not a platform C ABI.");
            return 0;
        } else if (arg[0] == '-' && (arg[1] == 'I' || arg[1] == 'D' || arg[1] == 'U')) {
            append(options, &option_count, arg);
            if (!arg[2]) {
                if (i + 1 >= argc)
                    die("missing preprocessing argument");
                append(options, &option_count, argv[++i]);
            }
        } else if (arg[0] == '-')
            die("unsupported option; use --help");
        else
            append(inputs, &input_count, arg);
    }
    if (!input_count)
        die("no source/object input");
    if (stage != 5 && input_count != 1)
        die("stop modes require one source input");
    if (!output && stage == 4) {
        const char *base = strrchr(inputs[0], '/');
        char *dot;
        base = base ? base + 1 : inputs[0];
        output = malloc(strlen(base) + 3);
        if (!output)
            die("out of memory");
        strcpy(output, base);
        dot = strrchr(output, '.');
        if (dot)
            *dot = 0;
        strcat(output, ".o");
    }
    if (!output)
        output = stage == 0   ? "a.i"
                 : stage == 1 ? "a.ir"
                 : stage == 2 ? "a.ssa"
                 : stage == 3 ? "a.s"
                              : "a.out";
    for (i = 0; i < input_count; i++)
        if (!strcmp(inputs[i], output))
            die("output must not overwrite input");
    directory = malloc(80);
    if (!directory)
        die("out of memory");
    sprintf(directory, "/tmp/rcc-native-%u", (unsigned)getpid());
    if (mkdir(directory, 0700))
        die("cannot create private temporary directory");
    atexit(cleanup);
    for (i = 0; i < input_count; i++) {
        size_t length = strlen(inputs[i]);
        int object = length >= 2 && (!strcmp(inputs[i] + length - 2, ".o") ||
                                     !strcmp(inputs[i] + length - 2, ".a"));
        if (object && stage != 5)
            die("stop modes require C source, not object input");
        append(objects, &object_count, object ? inputs[i] : compile(inputs[i], output, stage));
        if (stage != 5)
            return 0;
    }
    append(command, &n, "rcc-x64-ld");
    if (sdk) {
        append(command, &n, "--sdk");
        append(command, &n, (char *)sdk);
    }
    for (i = 0; i < object_count; i++)
        append(command, &n, objects[i]);
    append(command, &n, "-o");
    append(command, &n, output);
    run(command);
    return 0;
}
