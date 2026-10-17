/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Built only by test_timers.py, never installed on the user's disk. */
#include <stdio.h>
#include <unistd.h>
#include <signal.h>
#include <time.h>
#include <string.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/times.h>
extern long call(unsigned, long, long, long, long);
static volatile unsigned caught;
static void alarmed(int sig) { (void)sig; ++caught; signal(SIGALRM, alarmed); }
static unsigned long monotonic(void) {
    __ktime_t t;
    if (_time(&t, 1) < 0) return 0xffffffffUL;
    return t.low;
}
int main(int argc, char **argv) {
    __ktime_t before, changed, after;
    unsigned long start, end;
    struct stat st;
    struct tms cpu_before, cpu_after;
    int fd;
    signal(SIGALRM, alarmed);
    if (argc > 1 && !strcmp(argv[1], "--pause")) {
        if (times(&cpu_before)) return 11;
        start = monotonic();
        alarm(1);
        puts("CLOCK_PAUSE_READY"); fflush(stdout);
        while (!caught) ; /* Must receive the alarm without making syscalls. */
        end = monotonic();
        if (times(&cpu_after)) return 12;
        printf("CLOCK_PAUSED:%lu SIGNALS:%u CPU:%lu\n", end - start, caught,
               cpu_after.tms_utime + cpu_after.tms_stime - cpu_before.tms_utime - cpu_before.tms_stime);
        return caught != 1 || end - start < 25 ||
            cpu_after.tms_utime + cpu_after.tms_stime - cpu_before.tms_utime - cpu_before.tms_stime > 50;
    }
    _time(&before, 0);
    printf("CLOCK_EPOCH:%lu\n", before.low);
    /* Calendar changes must not move uptime or monotonic deadlines. */
    start = monotonic();
    changed.low = 123456789UL; changed.high = 0;
    if (call(28, (long)&changed, 0, 0, 0) < 0) return 1;
    _time(&after, 0);
    if (after.low < changed.low || after.low > changed.low + 1 ||
        monotonic() - start > 10) return 2;
    puts("CLOCK_SLEEP_READY"); fflush(stdout);
    if (sleep(2)) return 3;
    end = monotonic();
    printf("CLOCK_SLEEP:%lu\n", end - start);
    if (end - start < 19 || end - start > 50) return 4;
    _time(&after, 0);
    if (after.low < changed.low + 1 || after.low > changed.low + 5) return 5;
    before.low += (end - start) / 10;
    if (call(28, (long)&before, 0, 0, 0) < 0) return 6;
    start = monotonic(); alarm(1);
    while (!caught) ;
    end = monotonic();
    printf("CLOCK_ALARM:%lu\n", end - start);
    if (end - start < 9 || end - start > 30 || caught != 1) return 7;
    alarm(1); alarm(0);
    sleep(2);
    if (caught != 1) return 8;
    fd = open("/clock-stamp", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0 || write(fd, "clock\n", 6) != 6 || close(fd)) return 9;
    _time(&after, 0);
    if (stat("/clock-stamp", &st) || st.st_mtime < before.low ||
        st.st_mtime > after.low + 1) return 10;
    puts("clockcheck: sleep, alarm, cancellation, calendar isolation and timestamps passed");
    return 0;
}
