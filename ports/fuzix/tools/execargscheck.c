/* SPDX-License-Identifier: GPL-2.0-only */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static char text[5000], environment[3000];
static char *vector[1500];

static int rejected(char **args, char **env) {
    execve("/bin/execargscheck", args, env);
    return errno == E2BIG ? 0 : 1;
}

int main(int argc, char **argv) {
    char *args[5], *env[2], *value;
    int j, child, status, failed = 0;
    if (argc == 4) {
        value = getenv("PROBE");
        if (!value || strlen(value) != 900 || strlen(argv[2]) != 900 || strlen(argv[3]) != 900) return 10;
        for (j = 0; j < 900; j++) if (argv[2][j] != argv[3][j] || value[j] != argv[2][j]) return 11;
        return 0;
    }
    for (j = 0; j < 4; j++) {
        child = fork();
        if (child < 0) return 20;
        if (!child) {
            memset(text, 'A' + j, 900); text[900] = 0;
            strcpy(environment, "PROBE="); memset(environment + 6, 'A' + j, 900); environment[906] = 0;
            args[0] = "execargscheck"; args[1] = "child"; args[2] = args[3] = text; args[4] = NULL;
            env[0] = environment; env[1] = NULL;
            execve("/bin/execargscheck", args, env); _exit(21);
        }
    }
    for (j = 0; j < 4; j++) {
        if (wait(&status) < 0 || !WIFEXITED(status) || WEXITSTATUS(status)) failed = 1;
    }
    if (failed) return 22;
    env[0] = NULL;
    memset(text, 'x', sizeof(text) - 1); text[sizeof(text) - 1] = 0;
    args[0] = "execargscheck"; args[1] = text; args[2] = NULL;
    if (rejected(args, env)) return 30;
    text[2000] = 0; memset(environment, 'e', 2600); environment[2600] = 0;
    env[0] = environment; env[1] = NULL;
    if (rejected(args, env)) return 31;
    for (j = 0; j < 1400; j++) vector[j] = "";
    vector[1400] = NULL; env[0] = NULL;
    if (rejected(vector, env)) return 32;
    puts("exec argument checks passed");
    return 0;
}
