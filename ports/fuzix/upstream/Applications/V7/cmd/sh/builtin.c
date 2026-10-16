/* UNIX V7 source code: see /COPYRIGHT or www.tuhs.org for details. */

#include "defs.h"

int builtin(int argn, STRING *cmd)
{
    if (jobs_builtin(argn, cmd)) return 1;
	if (eq(cmd[0], "shutdown")) {
        uadmin(1, 0, NULL);
        error("shutdown failed");
    }
    if (eq(cmd[0], "sync")) {
        sync();
        prs("synced\n");
        return 1;
    }
    if (eq(cmd[0], "help")) {
        prs("Shell: cd, set, export, read, eval, trap, wait, jobs, fg, bg, exit, shutdown.\n");
        prs("Scripts: if/for/while/case, pipes, redirection, globbing, `commands`.\n");
        return 1;
    }
    return 0;
}
