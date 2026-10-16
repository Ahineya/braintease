#include <native_process.h>

/* Compile once and time the executable. Exercises fork/wait and suspended
 * frames without console, graphics, host files, or networking. */
static int nested(int depth, int seed)
{
    int live = seed * 3 + depth;
    int dead = seed * 7;
    if (dead == -1)
        return 99;
    if (depth)
        return nested(depth - 1, seed) + live;
    int pid = rcc_process_fork();
    if (pid == 0)
        rcc_process_exit(live);
    int status;
    return rcc_process_wait(pid, &status) == pid && status == live ? live : -1000;
}

int main(void)
{
    for (int i = 0; i < 100; i++)
        if (nested(16, 3) != 289)
            return 1;
    return 0;
}
