/* RFL's OS boundary. All I/O, allocation and exit use guest libc/syscalls. */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <dirent.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <signal.h>
#include <termios.h>
extern int _argc;
extern char **_argv;
#undef putchar
#undef getchar
/* Guest stdio exposes these as macros; RFL calls real C ABI functions. */
int putchar(int ch) { return fputc(ch, stdout); }
int getchar(void) { return fgetc(stdin); }

/* A language failure must terminate this process, never the VM/host kernel. */
void rfl_halt(void) { exit(1); }
int rfl_posix_errno(void) { return errno; }
int rfl_posix_open(const unsigned char *path, int kind) {
    int flags = kind == 0 ? O_RDONLY : O_WRONLY | O_CREAT |
        (kind == 1 ? O_TRUNC : kind == 3 ? O_EXCL : O_APPEND);
    return open((const char *)path, flags, 0644);
}
int rfl_posix_close(int fd) { return close(fd); }
int rfl_posix_dup(int fd) { return dup(fd); }
long rfl_posix_read(int fd, unsigned char *data, unsigned long count) {
    long result;
    do { result = read(fd, data, count); } while (result < 0 && errno == EINTR);
    return result;
}
long rfl_posix_write(int fd, const unsigned char *data, unsigned long count) {
    long result;
    do { result = write(fd, data, count); } while (result < 0 && errno == EINTR);
    return result;
}
long rfl_posix_seek(int fd, long offset, int whence) { return lseek(fd, offset, whence); }
int rfl_posix_unlink(const unsigned char *path) { return unlink((const char *)path); }
int rfl_posix_rename(const unsigned char *from, const unsigned char *to) {
    return rename((const char *)from, (const char *)to);
}
int rfl_posix_getpid(void) { return getpid(); }
unsigned rfl_posix_argc(void) { return (unsigned)_argc; }
const unsigned char *rfl_posix_argument(unsigned index) {
    return index < (unsigned)_argc ? (const unsigned char *)_argv[index] : 0;
}
const unsigned char *rfl_posix_getenv(const unsigned char *name) {
    return (const unsigned char *)getenv((const char *)name);
}
void rfl_posix_exit(int status) { exit(status); }
const unsigned char *rfl_posix_error_message(int code) { return (const unsigned char *)strerror(code); }
void *rfl_posix_opendir(const unsigned char *path) { return opendir((const char *)path); }
int rfl_posix_closedir(void *dir) { return closedir(dir); }
int rfl_posix_readdir(void *dir, unsigned char *name) {
    struct dirent *entry;
    errno = 0;
    entry = readdir(dir);
    if (!entry) return errno ? -1 : 0;
    memcpy(name, entry->d_name, 31);
    name[30] = 0;
    return 1;
}
int rfl_posix_metadata(const unsigned char *path, unsigned long *data) {
    struct stat s;
    if (lstat((const char *)path, &s) < 0) return -1;
    data[0] = S_ISDIR(s.st_mode) ? 1 : S_ISREG(s.st_mode) ? 2 : S_ISLNK(s.st_mode) ? 3 : 0;
    data[1] = (unsigned long)s.st_size;
    data[2] = s.st_dev;
    data[3] = s.st_ino;
    data[4] = s.st_mode & 07777;
    return 0;
}
int rfl_posix_mkdir(const unsigned char *path) { return mkdir((const char *)path, 0755); }
int rfl_posix_rmdir(const unsigned char *path) { return rmdir((const char *)path); }
int rfl_posix_chdir(const unsigned char *path) { return chdir((const char *)path); }
const unsigned char *rfl_posix_cwd(void) {
    static char path[512];
    return (const unsigned char *)getcwd(path, sizeof(path));
}
/* Keep the foreground child in our terminal group. The parent ignores terminal
 * interrupts while waiting; the child restores the inherited dispositions. */
static long foreground(const char *program, char **argv, unsigned count) {
    pid_t child;
    int status, saved, i;
    pid_t terminal_group = tcgetpgrp(STDIN_FILENO);
    sighandler_t interrupt = signal(SIGINT, SIG_IGN), quit = signal(SIGQUIT, SIG_IGN);
    fflush(NULL);
    child = fork();
    if (!child) {
        signal(SIGINT, interrupt); signal(SIGQUIT, quit);
        execv(program, argv);
        if (errno == ENOEXEC) {
            /* The kernels load binary images. Let the guest shell interpret
             * executable text files, without parsing the pathname as syntax. */
            for (i = count; i > 0; --i) argv[i] = argv[i - 1];
            argv[0] = "sh"; argv[1] = (char *)program; argv[count + 1] = NULL;
            execv("/bin/sh", argv);
        }
        saved = errno; perror(program); _exit(saved == ENOENT ? 127 : 126);
    }
    if (child < 0) status = -1;
    else {
        while (waitpid(child, &status, 0) < 0) {
            if (errno != EINTR) { status = -1; break; }
        }
    }
    saved = errno;
    /* An interactive subshell creates its own foreground process group. Take
     * the terminal back before curses resumes, even though we are temporarily
     * a background group while making this ioctl. */
    if (terminal_group > 0) {
        sighandler_t output = signal(SIGTTOU, SIG_IGN);
        tcsetpgrp(STDIN_FILENO, terminal_group);
        signal(SIGTTOU, output);
    }
    signal(SIGINT, interrupt); signal(SIGQUIT, quit);
    errno = saved;
    return status == -1 ? -1L : (unsigned)status;
}
long rfl_posix_system(const unsigned char *command) {
    char *argv[5] = {"sh", "-c", (char *)command, NULL, NULL};
    return foreground("/bin/sh", argv, 3);
}
long rfl_posix_program(unsigned char *packed, unsigned count) {
    char **argv; unsigned i; long status; int saved;
    if (!count || count > 255) { errno = EINVAL; return -1; }
    argv = calloc(count + 2, sizeof(char *));
    if (!argv) { errno = ENOMEM; return -1; }
    for (i = 0; i < count; ++i) { argv[i] = (char *)packed; packed += strlen((char *)packed) + 1; }
    status = foreground(argv[0], argv, count);
    saved = errno; free(argv); errno = saved; return status;
}

int rfl_posix_chmod(const unsigned char *path, unsigned mode) { return chmod((const char *)path, mode); }
int rfl_posix_executable(const unsigned char *path) {
    struct stat s;
    if (stat((const char *)path, &s) < 0) return -1;
    if (!S_ISREG(s.st_mode) || !(s.st_mode & 0111)) return 0;
    if (access((const char *)path, X_OK) == 0) return 1;
    return errno == EACCES ? 0 : -1;
}
