/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <termios.h>
#include <sys/wait.h>
#include "resources.h"
extern long call(unsigned, long, long, long, long);
static void require(int ok, const char *what) {
    if (!ok) { fprintf(stderr, "ttycheck: FAILED %s (errno %d)\n", what, errno); exit(1); }
}
static int getstatus(int child, int flags) {
    int s, pid;
    do pid = waitpid(child, &s, flags); while (pid < 0 && errno == EINTR);
    require(pid == child, "waitpid"); return s;
}
int main(int argc, char **argv) {
    if (argc == 2 && !strcmp(argv[1], "--wait")) {
        puts("TTY_WAIT_READY");
        for (;;) pause();
    }
    int p, status, m, m2, slave, fd, n;
    char buf[600];
    struct termios t;
    struct ripple_procinfo info;
    struct ripple_sysinfo sys;
    int pipefd[2];
    struct { unsigned count, canary; } queued;
    fd = open("/dev/proc", O_RDONLY);
    require(fd >= 0 && !ioctl(fd, RIPPLE_SYSINFO, &sys), "sysinfo");
    for (n = 0; n < sys.slots; n++) {
        info.slot = n;
        require(!ioctl(fd, RIPPLE_PROCINFO, &info), "process record");
        require(info.version == 1 && info.cpu_hz == 100 &&
                info.mapped_kib == info.private_kib + info.shared_kib, "process accounting");
        if (info.pid == getpid()) require(info.pgrp == getpgrp() && info.session == getsid(0), "group/session reporting");
    }
    info.slot = sys.slots;
    require(ioctl(fd, RIPPLE_PROCINFO, &info) < 0 && errno == EINVAL, "invalid process slot");
    close(fd);
    p = fork(); require(p >= 0, "fork stop child");
    if (!p) { kill(getpid(), SIGSTOP); _exit(7); }
    status = getstatus(p, WUNTRACED);
    require(WIFSTOPPED(status) && WSTOPSIG(status) == SIGSTOP, "WUNTRACED stop encoding");
    fd = open("/dev/proc", O_RDONLY); require(fd >= 0, "open shared-page report");
    for (n = 0; n < sys.slots; n++) {
        info.slot = n; require(!ioctl(fd, RIPPLE_PROCINFO, &info), "fork memory report");
        if (info.pid == p) require(info.mapped_kib == info.private_kib + info.shared_kib &&
            (!(sys.flags & RIPPLE_SYSINFO_MMU) || info.shared_kib > 0), "shared fork pages");
    }
    close(fd);
    require(!kill(p, SIGCONT), "continue");
    status = getstatus(p, 0); require(WIFEXITED(status) && WEXITSTATUS(status) == 7, "resumed child");
    p = fork(); require(p >= 0, "fork killed child");
    if (!p) { kill(getpid(), SIGSTOP); _exit(99); }
    require(WIFSTOPPED(getstatus(p, WUNTRACED)), "stop before kill");
    require(!kill(p, SIGKILL), "kill stopped child");
    require(WIFSIGNALED(getstatus(p, 0)), "stopped child reaped");
    puts("ttycheck: stop/continue, stopped kill, and process snapshots passed");
    m = open("/dev/ptyp0", O_RDWR | O_NONBLOCK | O_NOCTTY);
    require(m >= 0, "open master");
    require(open("/dev/ptyp0", O_RDWR) < 0 && errno == EBUSY, "exclusive master");
    slave = open("/dev/ttyp0", O_RDWR | O_NONBLOCK | O_NOCTTY);
    require(slave >= 0, "open slave");
    require(!tcgetattr(slave, &t), "slave termios");
    t.c_lflag &= ~ECHO; require(!tcsetattr(slave, TCSANOW, &t), "disable echo");
    require(write(m, "abc", 3) == 3, "master input");
    queued.count = 0; queued.canary = 0xcafe;
    require(!ioctl(slave, TIOCINQ, &queued.count) && queued.count == 3 && queued.canary == 0xcafe, "input queue ioctl width");
    require(read(slave, buf, sizeof(buf)) < 0 && errno == EAGAIN, "canonical incomplete line");
    require(write(m, "\n", 1) == 1 && read(slave, buf, sizeof(buf)) == 4 && !memcmp(buf, "abc\n", 4), "canonical complete line");
    require(write(slave, "hello\n", 6) == 6, "slave output");
    queued.count = 0; queued.canary = 0xcafe;
    require(!ioctl(m, TIOCINQ, &queued.count) && queued.count == 7 && queued.canary == 0xcafe, "output queue ioctl width");
    require(read(m, buf, sizeof(buf)) == 7 && !memcmp(buf, "hello\r\n", 7), "output conversion");
    m2 = open("/dev/ptyp1", O_RDWR | O_NONBLOCK);
    require(m2 >= 0 && write(m2, "other\n", 6) == 6, "independent second PTY");
    require(read(slave, buf, sizeof(buf)) < 0 && errno == EAGAIN, "input isolation");
    close(m2);
    memset(buf, 'c', sizeof(buf));
    require(write(m, buf, sizeof(buf)) == sizeof(buf) && write(m, "\n", 1) == 1,
            "overlong canonical input remains terminable");
    require(read(slave, buf, sizeof(buf)) == 512 && buf[511] == '\n', "bounded canonical line");
    t.c_lflag &= ~ICANON; t.c_cc[VMIN] = 1; t.c_cc[VTIME] = 0; t.c_oflag = 0;
    require(!tcsetattr(slave, TCSANOW, &t), "raw slave");
    memset(buf, 'x', sizeof(buf));
    require(write(slave, buf, sizeof(buf)) == 512, "output backpressure partial write");
    require(write(slave, buf, 1) < 0 && errno == EAGAIN, "nonblocking full output");
    require(read(m, buf, sizeof(buf)) == 512 && write(slave, "y", 1) == 1 && read(m, buf, sizeof(buf)) == 1 && buf[0] == 'y', "output recovery");
    require(write(m, buf, sizeof(buf)) == 512, "input backpressure partial write");
    require(write(m, buf, 1) < 0 && errno == EAGAIN, "nonblocking full input");
    require(read(slave, buf, 512) == 512, "input recovery");
    close(slave);
    require(read(m, buf, sizeof(buf)) == 0, "last slave close gives EOF");
    close(m);
    m = open("/dev/ptyp0", O_RDWR | O_NONBLOCK);
    require(m >= 0, "reuse master");
    slave = open("/dev/ttyp0", O_RDWR | O_NOCTTY);
    require(slave >= 0, "reuse slave");
    require(write(m, "partial", 7) == 7, "partial line before hangup");
    close(m);
    require(read(slave, buf, sizeof(buf)) == 7 && !memcmp(buf, "partial", 7), "hangup drains partial line");
    require(read(slave, buf, 1) < 0, "master close hangs up slave");
    close(slave);
    puts("ttycheck: PTY isolation, canonical input, backpressure, EOF, hangup, reuse passed");
    m = open("/dev/ptyp0", O_RDWR | O_NONBLOCK);
    require(m >= 0 && !pipe(pipefd), "session setup");
    p = fork(); require(p >= 0, "session fork");
    if (!p) {
        struct winsize size = {30, 90, 0, 0};
        int childpid = getpid(), oldsid = getsid(0), ct;
        close(m); close(pipefd[0]);
        require(setsid() == childpid && getsid(0) == childpid && oldsid != childpid, "new session");
        ct = open("/dev/tty", O_RDWR); require(ct < 0, "detached controlling tty");
        slave = open("/dev/ttyp0", O_RDWR); require(slave >= 0, "acquire slave");
        require(tcgetpgrp(slave) == childpid && tcsetpgrp(slave, getppid()) < 0 && errno == EPERM, "session foreground isolation");
        ct = open("/dev/tty", O_RDWR); require(ct >= 0, "controlling tty alias");
        require(!ioctl(ct, TIOCSWINSZ, &size), "window size");
        write(pipefd[1], "y", 1); close(pipefd[1]); close(ct); close(slave); _exit(0);
    }
    close(pipefd[1]);
    require(read(pipefd[0], buf, 1) == 1, "session child result"); close(pipefd[0]);
    status = getstatus(p, 0); require(WIFEXITED(status) && !WEXITSTATUS(status), "session child exit");
    close(m);
    puts("ttycheck: controlling terminals and session isolation passed");
    return 0;
}
