/* Guest compiler driver. Every compilation stage is an ordinary guest process. */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#define MAX_ARGUMENTS 256

enum compile_mode { LINK_EXECUTABLE, COMPILE_OBJECT, EMIT_ASSEMBLY, PREPROCESS_ONLY };

static char *temporaries[MAX_ARGUMENTS], *preprocessor_arguments[MAX_ARGUMENTS],
    *inputs[MAX_ARGUMENTS], *objects[MAX_ARGUMENTS];
static int temporary_count, preprocessor_count, input_count, object_count, verbose;
static const char *sdk, *tools;

static void cleanup(void)
{
    int i;
    for (i = 0; i < temporary_count; i++)
        unlink(temporaries[i]);
}

static void die(const char *message)
{
    fprintf(stderr, "cc: %s\n", message);
    exit(1);
}

static char *join_path(const char *base, const char *name)
{
    char *p = malloc(strlen(base) + strlen(name) + 2);
    if (!p)
        die("out of memory");
    sprintf(p, "%s/%s", base, name);
    return p;
}

static char *temporary_path(const char *suffix)
{
    char *p = malloc(80);
    if (!p)
        die("out of memory");
    if (temporary_count >= MAX_ARGUMENTS)
        die("too many temporary files");
    sprintf(p, "/tmp/rcc-%u-%u.%s", (unsigned)getpid(), (unsigned)temporary_count, suffix);
    temporaries[temporary_count++] = p;
    return p;
}

static void append_argument(char **argv, int *n, char *value)
{
    if (*n >= MAX_ARGUMENTS - 1)
        die("too many arguments");
    argv[(*n)++] = value;
    argv[*n] = NULL;
}

static void run_stage(char **argv)
{
    pid_t child;
    int status, i;
    const char *tool = argv[0];
    char *program = tools ? join_path(tools, tool) : argv[0];
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
        fprintf(stderr, "cc: cannot execute %s\n", program);
        _exit(127);
    }
    while (waitpid(child, &status, 0) < 0)
        if (errno != EINTR)
            die("cannot wait for compiler stage");
    if (!WIFEXITED(status)) {
        fprintf(stderr, "cc: %s terminated by signal %u\n", tool, (unsigned)WTERMSIG(status));
        exit(1);
    }
    if (WEXITSTATUS(status))
        exit(WEXITSTATUS(status));
}

static int has_suffix(const char *name, const char *suffix)
{
    size_t a = strlen(name), b = strlen(suffix);
    return a >= b && !strcmp(name + a - b, suffix);
}

/* Source compilation is a fixed pipeline. Early modes stop after one stage. */
static char *compile_source(char *source, const char *output, enum compile_mode mode)
{
    char *ir, *assembly, *object;
    char *command[MAX_ARGUMENTS];
    char *include = join_path(sdk, "include");
    int n = 0, j;
    ir = mode == PREPROCESS_ONLY ? (char *)output : temporary_path("ir");
    assembly = mode == EMIT_ASSEMBLY ? (char *)output : temporary_path("asm");
    object = mode == COMPILE_OBJECT ? (char *)output : temporary_path("rpo");
    append_argument(command, &n, "rcc-c");
    for (j = 0; j < preprocessor_count; j++)
        append_argument(command, &n, preprocessor_arguments[j]);
    append_argument(command, &n, "-I");
    append_argument(command, &n, include);
    if (mode == PREPROCESS_ONLY)
        append_argument(command, &n, "-E");
    append_argument(command, &n, source);
    append_argument(command, &n, "-o");
    append_argument(command, &n, ir);
    run_stage(command);
    if (mode == PREPROCESS_ONLY)
        return NULL;
    n = 0;
    append_argument(command, &n, "rcc-ripple");
    append_argument(command, &n, "--unit");
    append_argument(command, &n, source);
    append_argument(command, &n, ir);
    append_argument(command, &n, "-o");
    append_argument(command, &n, assembly);
    run_stage(command);
    if (mode == EMIT_ASSEMBLY)
        return NULL;
    n = 0;
    append_argument(command, &n, "rcc-as");
    append_argument(command, &n, assembly);
    append_argument(command, &n, "-o");
    append_argument(command, &n, object);
    run_stage(command);
    if (mode == COMPILE_OBJECT)
        return NULL;
    return object;
}

static void link_program(const char *output, int bare)
{
    char *command[MAX_ARGUMENTS];
    int n = 0, i;
    mode_t mask;
    append_argument(command, &n, "rcc-ld");
    append_argument(command, &n, "-o");
    append_argument(command, &n, (char *)output);
    if (bare) {
        append_argument(command, &n, "-f");
        append_argument(command, &n, "binary");
    }
    append_argument(command, &n, join_path(sdk, bare ? "bare.rpo" : "entry.rpo"));
    for (i = 0; i < object_count; i++)
        append_argument(command, &n, objects[i]);
    append_argument(command, &n, join_path(sdk, bare ? "runtime.rpa" : "libc.rpa"));
    run_stage(command);
    mask = umask(0);
    umask(mask);
    if (chmod(output, (mode_t)(0777 & ~mask)))
        die("cannot mark output executable");
}

int main(int argc, char **argv)
{
    const char *output = NULL;
    int i, bare = 0;
    enum compile_mode mode = LINK_EXECUTABLE;
    sdk = getenv("RCC_SDK");
    if (!sdk || !*sdk)
        sdk = "/usr/lib/rcc";
    tools = getenv("RCC_TOOLS");
    if (tools && !*tools)
        tools = NULL;
    atexit(cleanup);
    for (i = 1; i < argc; i++) {
        char *arg = argv[i];
        if (!strcmp(arg, "-o") && i + 1 < argc)
            output = argv[++i];
        else if (!strcmp(arg, "-c"))
            mode = COMPILE_OBJECT;
        else if (!strcmp(arg, "-S"))
            mode = EMIT_ASSEMBLY;
        else if (!strcmp(arg, "-E"))
            mode = PREPROCESS_ONLY;
        else if (!strcmp(arg, "-v"))
            verbose = 1;
        else if (!strcmp(arg, "--bare"))
            bare = 1;
        else if (!strcmp(arg, "--sdk") && i + 1 < argc)
            sdk = argv[++i];
        else if (!strcmp(arg, "--tools") && i + 1 < argc)
            tools = argv[++i];
        else if (arg[0] == '-' && (arg[1] == 'I' || arg[1] == 'D' || arg[1] == 'U')) {
            append_argument(preprocessor_arguments, &preprocessor_count, arg);
            if (!arg[2]) {
                if (i + 1 >= argc)
                    die("missing preprocessing option argument");
                append_argument(preprocessor_arguments, &preprocessor_count, argv[++i]);
            }
        } else if (arg[0] == '-')
            die("unsupported option (use -I, -D, -U, -c, -S, -E, -o, --sdk, --tools, --bare or "
                "-v)");
        else
            append_argument(inputs, &input_count, arg);
    }
    if (!input_count)
        die("no input files");
    if (mode && input_count != 1)
        die("-c, -S and -E currently require one input file");
    if (!output) {
        if (mode) {
            const char *s = strrchr(inputs[0], '/');
            char *name = malloc(strlen(s ? s + 1 : inputs[0]) + 8), *dot;
            if (!name)
                die("out of memory");
            strcpy(name, s ? s + 1 : inputs[0]);
            dot = strrchr(name, '.');
            if (dot)
                *dot = 0;
            strcat(name, mode == COMPILE_OBJECT ? ".rpo" : mode == EMIT_ASSEMBLY ? ".asm" : ".i");
            output = name;
        } else
            output = "a.out";
    }
    for (i = 0; i < input_count; i++)
        if (!strcmp(output, inputs[i]))
            die("output must not overwrite input");
    for (i = 0; i < input_count; i++) {
        char *object;
        if (has_suffix(inputs[i], ".rpo") || has_suffix(inputs[i], ".rpa")) {
            if (mode)
                die("source input required with -c/-S/-E");
            object = inputs[i];
        } else {
            object = compile_source(inputs[i], output, mode);
            if (mode)
                return 0;
        }
        append_argument(objects, &object_count, object);
    }
    link_program(output, bare);
    return 0;
}
