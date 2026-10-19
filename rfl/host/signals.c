/* Signal handlers only set a flag. Normal runtime checkpoints restore curses
 * and exit; atexit also restores the terminal on ordinary runtime failures. */
#include <signal.h>
#include <string.h>
static volatile sig_atomic_t interrupted;
static struct sigaction old_term, old_hup;
static int installed;
static void interrupt(int sig) { interrupted = sig; }
void rfl_host_tui_signals(int enabled) {
    struct sigaction action;
    if (enabled && !installed) {
        memset(&action, 0, sizeof(action));
        action.sa_handler = interrupt; sigemptyset(&action.sa_mask);
        sigaction(SIGTERM, &action, &old_term); sigaction(SIGHUP, &action, &old_hup);
        interrupted = 0; installed = 1;
    } else if (!enabled && installed) {
        sigaction(SIGTERM, &old_term, 0); sigaction(SIGHUP, &old_hup, 0);
        installed = 0;
    }
}
int rfl_host_tui_interrupted(void) { return interrupted; }
