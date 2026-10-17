/* Local port glue; Lynx/libwww still implement HTTP and HTML in the guest. */
#include <HTUtils.h>
#include <HTAlert.h>
#include <HTTCP.h>
#include <LYUtils.h>
#include <ripple_browser.h>
#include <curses.h>
#include <stdlib.h>
#include <stdio.h>
#include <signal.h>
#include <errno.h>
#include <unistd.h>

extern int no_suspend;
static char *heap_start;
static void report_resources(void)
{
    char *end = sbrk(0);
    if (end != (char *)-1)
        fprintf(stderr, "LYNX_HEAP_ARENA_CELLS:%lu\n", (unsigned long)(end - heap_start));
}
void ripple_browser_init(void)
{
    heap_start = sbrk(0);
    if (heap_start != (char *)-1 && getenv("LYNX_RESOURCE_REPORT"))
        atexit(report_resources);
}
/* Upstream expects ncurses to own SIGTSTP. Our shared curses deliberately
 * leaves signal policy to applications, so restore the shell before stopping
 * and repaint when the same handler resumes after fg/SIGCONT. */
static void suspend_browser(int sig)
{
    (void)sig;
    endwin();
    signal(SIGTSTP, SIG_DFL);
    kill(getpid(), SIGSTOP);
    signal(SIGTSTP, suspend_browser);
    reset_prog_mode();
    touchwin(stdscr);
    wrefresh(stdscr);
}
void ripple_browser_screen_init(void)
{
    if (!initscr()) {
        fputs("lynx: cannot initialize terminal; check /etc/termcap and terminal size.\n", stderr);
        exit(1);
    }
    if (!no_suspend) signal(SIGTSTP, suspend_browser);
}

static unsigned long received;
static int limit_hit;

void ripple_browser_begin(void)
{
    received = 0;
    limit_hit = 0;
}
int ripple_browser_limit_hit(void) { return limit_hit; }

int ripple_browser_read(int fd, void *buffer, unsigned capacity)
{
    unsigned long remaining = RIPPLE_BROWSER_PAGE_LIMIT - received;
    int count;
    /* Read one extra octet to distinguish exact-limit EOF from overflow.
     * This limit counts wire octets, including headers/chunk framing. */
    if ((unsigned long)capacity > remaining + 1)
        capacity = (unsigned)(remaining + 1);
    count = HTDoRead(fd, buffer, capacity);
    if (count > 0) {
        if ((unsigned long)count > remaining) {
            limit_hit = 1;
            HTAlert("Page exceeds the 64 KiB transfer limit.");
            return HT_INTERRUPTED;
        }
        received += (unsigned)count;
    }
    return count;
}

int ripple_browser_write(int fd, const void *buffer, unsigned size)
{
    const char *next = buffer;
    unsigned sent = 0;
    int count;
    while (sent < size) {
        if (HTCheckForInterrupt()) { errno = EINTR; return -1; }
        count = write(fd, next + sent, size - sent);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) { if (!count) errno = EPIPE; return -1; }
        sent += (unsigned)count;
    }
    return (int)sent;
}
