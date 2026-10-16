/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Interactive input for the V7 shell. No malloc: sh owns its brk arena. */
#include "defs.h"
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <termios.h>
#include <readline/readline.h>
extern SYSNOD commands[];
#define HISTORY 32
#define MATCHES 128
#define LINE 256
static char *hist;
static unsigned histcap, used, count, offsets[HISTORY];
static char draft[LINE];
static char matches[MATCHES][32];
static unsigned nmatches;
static int overflow;
static char *buf;
static unsigned end, cursor, capacity, columns, screenrow;
static int out;
static const char *ps;
static DIR directory;
static char decoded[LINE], dirname_buf[LINE], pathname[LINE];

static void put(const char *s) { write(out, s, strlen(s)); }
static void bell(void) { put("\007"); }
static void number(unsigned n, char suffix) {
    char s[12]; unsigned i = 10;
    s[11] = 0; s[10] = suffix;
    do { s[--i] = '0' + n % 10; n /= 10; } while (n);
    put("\033["); put(s + i);
}
static unsigned width(const char *s, unsigned n) {
    unsigned result = 0, esc = 0, i;
    for (i = 0; i < n; i++) {
        unsigned c = s[i];
        if (esc) { if (c >= '@' && c <= '~' && c != '[') esc = 0; }
        else if (c == 27) esc = 1;
        else result += c < 32 || c >= 127 ? 2 : 1;
    }
    return result;
}
static void text(const char *s, unsigned n) {
    unsigned i; char c[2];
    for (i = 0; i < n; i++) {
        if (s[i] < 32 || s[i] >= 127) {
            c[0] = '^'; c[1] = s[i] == 127 ? '?' : (s[i] & 31) + '@';
            write(out, c, 2);
        } else write(out, s + i, 1);
    }
}
static unsigned position(unsigned n) { return width(ps, strlen(ps)) + width(buf, n); }
static void redraw(void) {
    unsigned full, pos, row;
    put("\r");
    if (screenrow) number(screenrow, 'A');
    if (width(ps, strlen(ps)) < columns) {
        unsigned prefix = width(ps, strlen(ps));
        if (prefix) number(prefix, 'C');
        put("\033[J");
    } else { put("\033[J"); put(ps); }
    text(buf, end);
    full = position(end); pos = position(cursor);
    /* Resolve the terminal's pending wrap at an exact right margin. */
    if (full && full % columns == 0) put(" \b");
    row = full / columns;
    if (row > pos / columns) number(row - pos / columns, 'A');
    put("\r");
    if (pos % columns) number(pos % columns, 'C');
    screenrow = pos / columns;
}
void rl_hinit(char *buffer, size_t len) {
    hist = buffer; histcap = len; used = count = 0;
}
static void remember(void) {
    unsigned n = end + 1, remove, i;
    if (!hist || !end || n > histcap) return;
    if (count && !strcmp(hist + offsets[count - 1], buf)) return;
    while (count && (count == HISTORY || used + n > histcap)) {
        remove = strlen(hist) + 1;
        memmove(hist, hist + remove, used - remove); used -= remove;
        for (i = 1; i < count; i++) offsets[i - 1] = offsets[i] - remove;
        count--;
    }
    offsets[count++] = used; memcpy(hist + used, buf, n); used += n;
}
static void candidate(const char *name, const char *prefix, int isdir) {
    unsigned i, n = strlen(name);
    if (strncmp(name, prefix, strlen(prefix)) || n + isdir >= 32) return;
    if (name[0] == '.' && prefix[0] != '.') return;
    for (i = 0; i < nmatches; i++)
        if (!strncmp(matches[i], name, n) &&
            ((!isdir && !matches[i][n]) || (isdir && matches[i][n] == '/'))) return;
    if (nmatches == MATCHES) { overflow = 1; return; }
    strcpy(matches[nmatches], name);
    if (isdir) { matches[nmatches][n++] = '/'; matches[nmatches][n] = 0; }
    nmatches++;
}
static void complete_dir(const char *dir, const char *prefix, int executables, int dirs) {
    struct dirent *de;
    struct stat st;
    unsigned n = strlen(dir);
    if (n + 33 >= sizeof(pathname) || !opendir_r(&directory, dir)) return;
    strcpy(pathname, dir);
    if (n && pathname[n - 1] != '/') pathname[n++] = '/';
    while ((de = readdir(&directory))) {
        if (strncmp(de->d_name, prefix, strlen(prefix))) continue;
        strcpy(pathname + n, de->d_name);
        if (stat(pathname, &st)) continue;
        if (dirs && !S_ISDIR(st.st_mode)) continue;
        if (executables && (!S_ISREG(st.st_mode) || access(pathname, X_OK))) continue;
        candidate(de->d_name, prefix, S_ISDIR(st.st_mode));
    }
    closedir_r(&directory);
}
static int separator(char c) { return c == ';' || c == '|' || c == '&' || c == '(' || c == ')'; }
static int blankchar(char c) { return c == ' ' || c == '\t'; }
/* Decode only shell quoting; never evaluate expansions while completing. */
static void complete(int again) {
    unsigned i = 0, len = 0, common, j, addlen = 0;
    int quote = 0, command = 1, wascommand = 1, redir = 0, dirs = 0, unsafe = 0;
    char c, *base, *slash, addition[LINE];
    const char *path;
    if (cursor < end && !blankchar(buf[cursor]) && !separator(buf[cursor])) { bell(); return; }
    while (i < cursor) {
        c = buf[i++];
        if (!quote && (blankchar(c) || separator(c) || c == '<' || c == '>')) {
            if (len) {
                decoded[len] = 0;
                if (!redir) {
                    dirs = command && !strcmp(decoded, "cd");
                    if (!strchr(decoded, '=')) command = 0;
                    if (!strcmp(decoded, "then") || !strcmp(decoded, "do") ||
                        !strcmp(decoded, "else") || !strcmp(decoded, "if") ||
                        !strcmp(decoded, "while") || !strcmp(decoded, "until")) command = 1;
                }
                redir = 0;
            }
            if (separator(c)) { command = 1; dirs = 0; }
            if (c == '<' || c == '>') redir = 1;
            len = unsafe = 0; wascommand = command && !redir;
            continue;
        }
        if (c == '\\' && quote != '\'') {
            if (i == cursor) { bell(); return; }
            c = buf[i++];
        } else if (c == '\'' || c == '"') {
            if (!quote) { quote = c; continue; }
            if (quote == c) { quote = 0; continue; }
        } else if ((c == '$' || c == '`' || c == '*' || c == '?' || c == '[') && quote != '\'') unsafe = 1;
        if (len + 1 >= LINE) { bell(); return; }
        decoded[len++] = c;
    }
    decoded[len] = 0;
    if (unsafe) { bell(); return; }
    nmatches = overflow = 0;
    slash = strrchr(decoded, '/'); base = slash ? slash + 1 : decoded;
    if (slash) {
        unsigned d = slash - decoded + 1;
        memcpy(dirname_buf, decoded, d); dirname_buf[d] = 0;
        complete_dir(dirname_buf, base, 0, dirs && !redir);
    } else if (wascommand) {
        for (i = 0; commands[i].sysnam; i++) candidate(commands[i].sysnam, base, 0);
        candidate("help", base, 0); candidate("sync", base, 0); candidate("shutdown", base, 0);
        path = pathnod.namval ? pathnod.namval : "/bin";
        do {
            i = 0;
            while (*path && *path != ':') { if (i + 1 < LINE) dirname_buf[i++] = *path; path++; }
            dirname_buf[i] = 0;
            complete_dir(i ? dirname_buf : ".", base, 1, 0);
            if (!*path) break;
            path++;
        } while (1);
        complete_dir(".", base, 0, 1);
    } else complete_dir(".", base, 0, dirs && !redir);
    if (!nmatches) { bell(); return; }
    /* Sort without allocating, for repeatable listings. */
    for (i = 1; i < nmatches; i++) {
        char temp[32]; strcpy(temp, matches[i]); j = i;
        while (j && strcmp(matches[j - 1], temp) > 0) { strcpy(matches[j], matches[j - 1]); j--; }
        strcpy(matches[j], temp);
    }
    common = strlen(matches[0]);
    for (i = 1; i < nmatches; i++) {
        for (j = 0; j < common && matches[i][j] == matches[0][j]; j++);
        common = j;
    }
    /* A full match table cannot establish a unique/common completion. */
    if (overflow) common = strlen(base);
    for (i = strlen(base); i < common; i++) {
        c = matches[0][i];
        if (c < 32 || c >= 127) { bell(); return; }
        if (quote == '\'' && c == '\'') {
            if (addlen + 4 >= LINE) return;
            memcpy(addition + addlen, "'\\''", 4); addlen += 4;
        } else {
            if ((!quote && strchr(" \\'\"$`;&|()<>*?[]{}!#~", c)) ||
                (quote == '"' && strchr("\\\"$`", c))) addition[addlen++] = '\\';
            addition[addlen++] = c;
        }
        if (addlen + 4 >= LINE) { bell(); return; }
    }
    if (nmatches == 1 && !overflow && common && matches[0][common - 1] != '/') {
        if (quote) addition[addlen++] = quote;
        if (cursor == end || !blankchar(buf[cursor])) addition[addlen++] = ' ';
    }
    if (end + addlen > capacity) { bell(); return; }
    if (addlen) {
        memmove(buf + cursor + addlen, buf + cursor, end - cursor + 1);
        memcpy(buf + cursor, addition, addlen); cursor += addlen; end += addlen; redraw();
    } else if (again) {
        put("\n");
        for (i = 0; i < nmatches; i++) { text(matches[i], strlen(matches[i])); put("  "); }
        if (overflow) put("...");
        put("\n"); screenrow = 0; put(ps); text(buf, end);
        screenrow = position(end) / columns;
        redraw();
    } else bell();
}
int rl_edit(int fd, int ofd, const char *promptstr, char *inputbuf, size_t length) {
    struct termios saved, raw;
    struct winsize ws;
    unsigned hpos = count;
    int escape = 0, parameter = 0, previous = 0, result, cancelled = 0;
    char c;
    if (length > LINE) length = LINE;
    if (length < 2 || tcgetattr(fd, &saved)) return -1;
    raw = saved;
    raw.c_lflag &= ~(ICANON | ECHO | ISIG);
    raw.c_iflag &= ~(IXON | IXOFF);
    raw.c_cc[VMIN] = 1; raw.c_cc[VTIME] = 0;
    if (tcsetattr(fd, TCSANOW, &raw)) return -1;
    buf = inputbuf; end = cursor = 0; capacity = length - 1; buf[0] = 0;
    ps = promptstr; out = ofd; columns = 80;
    if (!ioctl(fd, TIOCGWINSZ, &ws) && ws.ws_col >= 20 && ws.ws_col <= 240) columns = ws.ws_col;
    put(ps); screenrow = position(0) / columns;
    while (1) {
        result = read(fd, &c, 1);
        if (result <= 0) { result = result < 0 && errno == EINTR ? -2 : -1; break; }
        if (escape) {
            if (escape == 1 && (c == '[' || c == 'O')) { escape = 2; parameter = 0; continue; }
            if (escape == 2 && c >= '0' && c <= '9') { parameter = parameter * 10 + c - '0'; continue; }
            if (escape == 2 && c == ';') { escape = 3; continue; }
            if (escape == 3 && c >= '0' && c <= '9') continue;
            escape = 0;
            if (c == 'A') c = 16;
            else if (c == 'B') c = 14;
            else if (c == 'C') c = 6;
            else if (c == 'D') c = 2;
            else if (c == 'H' || (c == '~' && (parameter == 1 || parameter == 7))) c = 1;
            else if (c == 'F' || (c == '~' && (parameter == 4 || parameter == 8))) c = 5;
            else if (c == '~' && parameter == 3) c = 4;
            else continue;
        }
        if (c == 27) { escape = 1; continue; }
        if (c == '\n' || c == '\r') { remember(); result = end; put("\n"); break; }
        if (c == 3) { put("^C\n"); cancelled = 1; exitval = 130; exitset(); result = -2; break; }
        if (c == 4 && !end) { put("\n"); result = -1; break; }
        if (c == '\t') { complete(previous == '\t'); previous = c; continue; }
        previous = c;
        if (c == 16 || c == 14) {
            if (hpos == count) strcpy(draft, buf);
            if (c == 16 && hpos) hpos--;
            else if (c == 14 && hpos < count) hpos++;
            else { bell(); continue; }
            strncpy(buf, hpos == count ? draft : hist + offsets[hpos], capacity); buf[capacity] = 0;
            end = cursor = strlen(buf); redraw(); continue;
        }
        if (c == 1) cursor = 0;
        else if (c == 5) cursor = end;
        else if (c == 2) { if (cursor) cursor--; }
        else if (c == 6) { if (cursor < end) cursor++; }
        else if (c == 4) { if (cursor < end) { memmove(buf + cursor, buf + cursor + 1, end - cursor); end--; } }
        else if (c == 8 || c == 127) {
            if (cursor) { cursor--; memmove(buf + cursor, buf + cursor + 1, end - cursor); end--; }
        } else if (c == 21) { end = cursor = 0; buf[0] = 0; }
        else if (c == 11) { end = cursor; buf[end] = 0; }
        else if (c == 23) {
            unsigned old = cursor;
            while (cursor && blankchar(buf[cursor - 1])) cursor--;
            while (cursor && !blankchar(buf[cursor - 1])) cursor--;
            memmove(buf + cursor, buf + old, end - old + 1); end -= old - cursor;
        } else if (c >= 32 && c < 127) {
            if (end == capacity) { bell(); continue; }
            memmove(buf + cursor + 1, buf + cursor, end - cursor + 1); buf[cursor++] = c; end++;
            if (cursor == end) {
                write(out, &c, 1);
                if (position(cursor) % columns == 0) put(" \b");
                screenrow = position(cursor) / columns; continue;
            }
        } else if (c != 12) continue;
        redraw();
    }
    tcsetattr(fd, TCSANOW, &saved);
    if (result == -2) {
        /* Restore the terminal before a trap executes or sigchk longjmps. */
        if (cancelled) fault(INTR);
        else put("\n");
        chktrap();
        sigchk();
    }
    return result;
}
