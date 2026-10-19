/* SPDX-License-Identifier: MIT */
/* Guest-only tools. A workspace is a working directory, not a sandbox. */
#include "chat.h"
#include "json.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

extern int setpgid(pid_t, pid_t);
static volatile sig_atomic_t cancelled, command_pid;
static void cancel(int sig) {
    (void)sig;
    cancelled = 1;
    if (command_pid > 0) {
        kill(-(pid_t)command_pid, SIGKILL);
        kill((pid_t)command_pid, SIGKILL);
    }
}
void chat_tools_init(void) {
    cancelled = command_pid = 0;
    setpgid(0, 0);
    signal(SIGTERM, cancel);
    signal(SIGINT, cancel);
}
int chat_tools_cancelled(void) { return cancelled; }

void chat_tool_definitions(FILE *f) {
    fputs("[", f);
    fputs("{\"type\":\"function\",\"name\":\"list_directory\",\"description\":\"List a guest directory; relative paths start at the workspace.\",\"strict\":true,\"parameters\":{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"}},\"required\":[\"path\"],\"additionalProperties\":false}},", f);
    fputs("{\"type\":\"function\",\"name\":\"read_file\",\"description\":\"Read guest UTF-8 text. Offset and next_offset count guest octets, not host storage bytes. Use offset=0 initially and max_bytes up to 32768.\",\"strict\":true,\"parameters\":{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"},\"offset\":{\"type\":\"integer\",\"minimum\":0},\"max_bytes\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":32768}},\"required\":[\"path\",\"offset\",\"max_bytes\"],\"additionalProperties\":false}},", f);
    fputs("{\"type\":\"function\",\"name\":\"write_file\",\"description\":\"Atomically create or replace a regular guest UTF-8 text file, up to 65536 octets. Does not follow a final symlink.\",\"strict\":true,\"parameters\":{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"},\"content\":{\"type\":\"string\"}},\"required\":[\"path\",\"content\"],\"additionalProperties\":false}},", f);
    fputs("{\"type\":\"function\",\"name\":\"edit_file\",\"description\":\"Replace exactly one occurrence of old_text in a regular UTF-8 guest file up to 65536 octets. Inspect the file first; ambiguous or missing text fails without changing it.\",\"strict\":true,\"parameters\":{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"},\"old_text\":{\"type\":\"string\"},\"new_text\":{\"type\":\"string\"}},\"required\":[\"path\",\"old_text\",\"new_text\"],\"additionalProperties\":false}},", f);
    fputs("{\"type\":\"function\",\"name\":\"run_command\",\"description\":\"Run a one-shot guest /bin/sh command in workdir (empty means workspace). No interactive stdin. Merged output is bounded to 32768 octets. timeout_seconds is 1..600. Descendants are stopped on completion, timeout or cancellation; persistent background jobs are not supported.\",\"strict\":true,\"parameters\":{\"type\":\"object\",\"properties\":{\"command\":{\"type\":\"string\"},\"workdir\":{\"type\":\"string\"},\"timeout_seconds\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":600}},\"required\":[\"command\",\"workdir\",\"timeout_seconds\"],\"additionalProperties\":false}}]", f);
}

/* Outputs are JSON strings inside function_call_output. Sanitize arbitrary
 * process octets to UTF-8 before quoting; never interpret terminal controls. */
static char *result(int ok, char *text, unsigned long length, int status,
                    int timed_out, int truncated, unsigned long next) {
    char *out = malloc(length * 6 + 256), *p;
    unsigned long i = 0, n;
    unsigned ch;
    if (!out) return NULL;
    p = out + sprintf(out, "{\"ok\":%s,\"exit_code\":%d,\"timed_out\":%s,\"interrupted\":%s,\"truncated\":%s,\"next_offset\":%lu,\"text\":\"",
        ok ? "true" : "false", status, timed_out ? "true" : "false", cancelled ? "true" : "false", truncated ? "true" : "false", next);
    while (i < length) {
        ch = (unsigned char)text[i];
        if (ch >= 128) {
            n = ch >= 194 && ch <= 223 ? 2 : ch >= 224 && ch <= 239 ? 3 : ch >= 240 && ch <= 244 ? 4 : 0;
            if (n && n <= length - i && cj_utf8(text + i, n)) {
                memcpy(p, text + i, n); p += n; i += n; continue;
            }
            *p++ = '?'; ++i; continue;
        }
        ++i;
        if (!ch) *p++ = '?';
        else if (ch == '"' || ch == '\\') { *p++ = '\\'; *p++ = ch; }
        else if (ch < 32 || ch == 127) p += sprintf(p, "\\u%04x", ch);
        else *p++ = ch;
    }
    strcpy(p, "\"}");
    return out;
}
static char *error(const char *text) {
    return result(0, (char *)text, strlen(text), -1, 0, 0, 0);
}
static char *string(CJ *j, const char *field) { return cj_string(j, cj_field(j, 0, field)); }
static int path_at(Chat *c, const char *path, char *out) {
    int n;
    if (!path || !*path) return 0;
    if (strlen(path) >= 500 || (path[0] != '/' && strlen(path) + strlen(c->workspace) + 1 >= 500)) return 0;
    n = path[0] == '/' ? snprintf(out, 512, "%s", path)
                        : snprintf(out, 512, "%s/%s", c->workspace, path);
    return n > 0 && n < 500;
}
static char *directory(const char *path) {
    DIR *dir = opendir(path);
    struct dirent *entry;
    struct stat st;
    char full[512], *text;
    unsigned long used = 0;
    unsigned count = 0;
    int truncated = 0, n;
    char *out;
    if (!dir) return error("Cannot open directory");
    text = malloc(CHAT_TOOL_TEXT + 1);
    if (!text) { closedir(dir); return NULL; }
    while ((entry = readdir(dir)) != NULL) {
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
        if (count++ == 256 || used + strlen(entry->d_name) + 48 > CHAT_TOOL_TEXT) { truncated = 1; break; }
        n = snprintf(full, sizeof(full), "%s/%s", path, entry->d_name);
        if (n < 0 || n >= (int)sizeof(full) || lstat(full, &st))
            used += sprintf(text + used, "? %s\n", entry->d_name);
        else
            used += sprintf(text + used, "%c %lu %s\n", S_ISDIR(st.st_mode) ? 'd' : S_ISLNK(st.st_mode) ? 'l' : 'f',
                            (unsigned long)st.st_size, entry->d_name);
    }
    closedir(dir);
    out = result(1, text, used, 0, 0, truncated, 0);
    free(text);
    return out;
}
static char *read_file(const char *path, CJ *args) {
    unsigned long offset, limit, n, drop = 0;
    struct stat st;
    FILE *file;
    char *text, *out;
    int more;
    if (!cj_number(args, cj_field(args, 0, "offset"), &offset) || offset > 0x7fffffffUL ||
        !cj_number(args, cj_field(args, 0, "max_bytes"), &limit) || !limit || limit > CHAT_TOOL_TEXT)
        return error("Invalid offset or max_bytes (1..32768)");
    if (stat(path, &st) || !S_ISREG(st.st_mode)) return error("Path is not a readable regular file");
    file = fopen(path, "r");
    if (!file) return error("Cannot open file");
    text = malloc(limit + 1);
    if (!text) { fclose(file); return NULL; }
    if (fseek(file, offset, SEEK_SET)) { free(text); fclose(file); return error("Cannot seek file"); }
    n = fread(text, 1, limit, file);
    more = fgetc(file) != EOF;
    if (ferror(file)) { free(text); fclose(file); return error("Cannot read file"); }
    fclose(file);
    if (memchr(text, 0, n)) { free(text); return error("File contains NUL; text tools do not read binary files"); }
    if (more) while (n && drop < 3 && !cj_utf8(text, n)) { --n; ++drop; }
    if (!cj_utf8(text, n) || (!n && more)) { free(text); return error("Not UTF-8 text, or offset/limit splits the first character"); }
    out = result(1, text, n, 0, 0, more, offset + n);
    free(text);
    return out;
}
static int replace_file(const char *path, const char *text) {
    struct stat st;
    char temp[512];
    const char *slash = strrchr(path, '/');
    int fd, ok, exists = lstat(path, &st) == 0;
    unsigned long n = strlen(text), at = 0, prefix = slash ? (unsigned long)(slash - path + 1) : 0;
    ssize_t count;
    if ((!exists && errno != ENOENT) || (exists && !S_ISREG(st.st_mode))) return 0;
    /* Keep the temporary's basename within Riplix's 30-cell directory limit. */
    if (prefix + sizeof(".chat-XXXXXX") > sizeof(temp)) return 0;
    memcpy(temp, path, prefix);
    strcpy(temp + prefix, ".chat-XXXXXX");
    fd = mkstemp(temp);
    if (fd < 0) return 0;
    ok = fchmod(fd, exists ? st.st_mode & 0777 : 0600) == 0;
    while (ok && at < n) {
        count = write(fd, text + at, n - at > 4096 ? 4096 : (unsigned)(n - at));
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) { ok = 0; break; }
        at += count;
    }
    if (close(fd)) ok = 0;
    if (ok) ok = rename(temp, path) == 0;
    if (!ok) unlink(temp);
    return ok;
}
static char *write_file(const char *path, CJ *args, int edit) {
    char *text = string(args, edit ? "new_text" : "content"), *old = NULL, *file = NULL, *joined = NULL, *at, *out;
    struct stat st;
    unsigned long n, prefix;
    if (!text || strlen(text) > 65536UL) { free(text); return error("Missing or oversized text (limit 65536 octets)"); }
    if (edit) {
        old = string(args, "old_text");
        if (lstat(path, &st) || !S_ISREG(st.st_mode) || !old || !*old ||
            !(file = chat_read(path, 65536UL)) || !cj_utf8(file, strlen(file))) {
            out = error("Cannot edit: need a regular UTF-8 file up to 65536 octets and nonempty old_text"); goto done;
        }
        at = strstr(file, old);
        if (!at || strstr(at + 1, old)) { out = error("old_text must match exactly once; file unchanged"); goto done; }
        n = strlen(file) - strlen(old) + strlen(text);
        if (n > 65536UL || !(joined = malloc(n + 1))) { out = error("Edited file exceeds limit or out of memory"); goto done; }
        prefix = at - file;
        memcpy(joined, file, prefix);
        strcpy(joined + prefix, text);
        strcpy(joined + prefix + strlen(text), at + strlen(old));
    }
    out = replace_file(path, edit ? joined : text) ? result(1, "File saved", 10, 0, 0, 0, 0)
                                                : error("Cannot save file; need a writable regular path (no final symlink)");
done:
    free(old); free(file); free(joined); free(text);
    return out;
}
static unsigned long seconds(void) {
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now)) return (unsigned long)time(NULL);
    return (unsigned long)now.tv_sec;
}
static char *command(Chat *c, CJ *args, int pipefd) {
    char *cmd = string(args, "command"), *cwd = string(args, "workdir"), full[512], chunk[1024], *text = NULL, *out;
    unsigned long timeout, started, used = 0, take, seen = 0;
    int pipes[2], nullfd, status = 0, ended = 0, eof = 0, timed_out = 0, truncated = 0;
    ssize_t n;
    pid_t pid, waited;
    if (!cmd || !*cmd || strlen(cmd) > CHAT_INPUT || !cwd ||
        !cj_number(args, cj_field(args, 0, "timeout_seconds"), &timeout) || !timeout || timeout > 600 ||
        !path_at(c, *cwd ? cwd : ".", full)) {
        free(cmd); free(cwd); return error("Invalid command, workdir or timeout_seconds (1..600)");
    }
    text = malloc(CHAT_TOOL_TEXT + 1);
    if (!text || pipe(pipes)) { free(text); free(cmd); free(cwd); return error("Cannot create command output pipe"); }
    pid = fork();
    if (!pid) {
        char *argv[] = {"sh", "-c", cmd, NULL};
        setpgid(0, 0);
        signal(SIGTERM, SIG_DFL); signal(SIGINT, SIG_DFL); signal(SIGPIPE, SIG_DFL);
        close(pipes[0]);
        nullfd = open("/dev/null", O_RDONLY);
        if (nullfd < 0 || dup2(nullfd, 0) < 0 || dup2(pipes[1], 1) < 0 || dup2(pipes[1], 2) < 0) _exit(126);
        if (nullfd > 2) close(nullfd);
        if (pipes[1] > 2) close(pipes[1]);
        if (pipefd > 2) close(pipefd);
        if (c->lock > 2) close(c->lock);
        if (chdir(full)) { fputs("Cannot enter command workdir\n", stderr); _exit(126); }
        execv("/bin/sh", argv);
        _exit(127);
    }
    close(pipes[1]);
    free(cmd); free(cwd);
    if (pid < 0) { close(pipes[0]); free(text); return error("No process slot for command"); }
    command_pid = pid;
    setpgid(pid, pid);
    fcntl(pipes[0], F_SETFL, O_NONBLOCK);
    started = seconds();
    while (!cancelled && (!ended || !eof)) {
        unsigned budget = 64;
        while (budget-- && (n = read(pipes[0], chunk, sizeof(chunk))) > 0) {
            take = (unsigned long)n;
            seen += take;
            if (take > CHAT_TOOL_TEXT - used) { take = CHAT_TOOL_TEXT - used; truncated = 1; }
            memcpy(text + used, chunk, take); used += take;
        }
        if (n == 0) eof = 1;
        if (!ended) {
            waited = waitpid(pid, &status, WNOHANG);
            if (waited == pid) ended = 1;
            else if (waited < 0 && errno != EINTR) { ended = 1; status = 127 << 8; }
        }
        if (ended) {
            /* Do not leave detached output writers or background descendants. */
            kill(-pid, SIGKILL);
        }
        if (seconds() - started >= timeout) { timed_out = 1; break; }
        if (!eof || !ended) usleep(20000UL);
    }
    kill(-pid, SIGKILL);
    if (!ended) {
        kill(pid, SIGKILL);
        while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    }
    command_pid = 0;
    close(pipes[0]);
    status = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + (WIFSIGNALED(status) ? WTERMSIG(status) : 0);
    out = result(!cancelled && !timed_out && status == 0, text, used, status, timed_out, truncated, seen);
    free(text);
    return out;
}

char *chat_tool_run(Chat *c, const char *name, const char *arguments, int fd) {
    CJ args;
    char *path, full[512], *out;
    if (!c->workspace[0]) return error("Tools are disabled in plain chats");
    if (cancelled) return error("Tool interrupted; inspect state before retrying effects");
    if (strlen(arguments) > 131072UL || !cj_parse(&args, arguments, strlen(arguments))) return error("Invalid tool arguments JSON");
    if (args.tokens[0].kind != '{') { cj_free(&args); return error("Tool arguments must be an object"); }
    if (!strcmp(name, "run_command")) out = command(c, &args, fd);
    else {
        path = string(&args, "path");
        if (!path_at(c, path, full)) out = error("Invalid or oversized path");
        else if (!strcmp(name, "list_directory")) out = directory(full);
        else if (!strcmp(name, "read_file")) out = read_file(full, &args);
        else if (!strcmp(name, "write_file") || !strcmp(name, "edit_file")) out = write_file(full, &args, !strcmp(name, "edit_file"));
        else out = error("Unknown tool name");
        free(path);
    }
    cj_free(&args);
    return out;
}
