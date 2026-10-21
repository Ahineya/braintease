#include "syma.h"
#include <errno.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>
#include <termios.h>
/* Effects stay in the immutable Program until their reply is ready. This roots
 * timer IDs and avoids background callbacks into a changed normalization
 * context. */
typedef struct Timer Timer;
struct Timer {
    Timer *next;
    Id request;
    double due;
};
static Timer *timers;
static Buffer console_input;
static Id input_request;
static struct termios saved_terminal;
static unsigned terminal_raw;
static int process_status;
void syma_effects_cleanup(void) {
    Timer *t, *next;
    if (terminal_raw) {
        tcsetattr(0, TCSANOW, &saved_terminal);
        terminal_raw = 0;
    }
    for (t = timers; t; t = next) {
        next = t->next;
        free(t);
    }
    timers = NULL;
    free(console_input.p);
    memset(&console_input, 0, sizeof(console_input));
    input_request = 0;
}
int syma_effect_status(void) { return process_status; }
static int pending_contains(Id pending, Id request) {
    Count i;
    for (i = 0; i < syma_argcnt(pending); i++)
        if (syma_arg(pending, i) == request)
            return 1;
    return 0;
}
static int textwrite(FILE *f, Id value) {
    Node *n = syma_node(value);
    Count i, bytes = 0;
    char *text = syma_utf8(value);
    for (i = 0; i < n->len; i++) {
        uint32_t c = n->s[i];
        if (c >= 0xd800 && c <= 0xdbff && i + 1 < n->len && n->s[i + 1] >= 0xdc00 &&
            n->s[i + 1] <= 0xdfff) {
            bytes += 4;
            i++;
        } else
            bytes += c < 128 ? 1 : c < 2048 ? 2 : 3;
    }
    int ok = fwrite(text, sizeof(char), bytes, f) == bytes;
    free(text);
    return ok;
}

static double realtime(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (double)tv.tv_sec * 1000 + (double)tv.tv_usec / 1000;
}
double syma_now(void) {
    char *fixed = getenv("SYMA_TEST_CLOCK");
    if (fixed)
        return strtod(fixed, NULL);
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (double)tv.tv_sec * 1000 + (double)tv.tv_usec / 1000;
}
static Id reply(const char *op, Id id, Id value) {
    return syma_named(op, syma_cat(syma_one(id), syma_one(value)));
}
static Id ioerror(void) { return syma_named("Error", syma_one(syma_str(strerror(errno)))); }
static Id remove_effect(Id program, Id request, Id response) {
    Id eff = syma_section(program, "Effects"), pending = syma_section(eff, "Pending"),
       inbox = syma_section(eff, "Inbox");
    Seq *keep = NULL;
    Count i;
    for (i = 0; i < syma_argcnt(pending); i++) {
        Id r = syma_arg(pending, i);
        if (syma_arg(r, 0) != syma_arg(request, 0))
            keep = syma_cat(keep, syma_one(r));
    }
    eff = syma_replace_section(eff, "Pending", syma_named("Pending", keep));
    eff = syma_replace_section(
        eff, "Inbox",
        syma_named("Inbox", syma_cat(inbox ? syma_node(inbox)->a : NULL, syma_one(response))));
    return syma_replace_section(program, "Effects", eff);
}
static char *storagepath(void) {
    char *home = getenv("HOME"), *p;
    if (!home || !*home)
        home = ".";
    p = syma_mem(((Count)strlen(home) + 32) * sizeof(char));
    strcpy(p, home);
    strcat(p, "/.syma");
    if (mkdir(p, 0700) && errno != EEXIST) {
        free(p);
        return NULL;
    }
    strcat(p, "/storage.syma");
    return p;
}
static Id storage(Id request) {
    Id id = syma_arg(request, 0), key = syma_arg(syma_arg(request, 2), 0),
       value = syma_arg(syma_arg(request, 3), 0), data, pair, found = 0;
    char *path = storagepath(), *text;
    FILE *f;
    Buffer content = {0};
    Seq *pairs = NULL;
    Count i;
    int c;
    if (!path)
        return reply("StorageGetComplete", id, ioerror());
    f = fopen(path, "r");
    if (f) {
        while ((c = fgetc(f)) != EOF)
            syma_bufput(&content, (unsigned)c);
        fclose(f);
        data = content.n ? syma_parse(content.p) : syma_named("Store", NULL);
    } else
        data = syma_named("Store", NULL);
    free(content.p);
    for (i = 0; i < syma_argcnt(data); i++) {
        pair = syma_arg(data, i);
        if (syma_arg(pair, 0) == key)
            found = syma_arg(pair, 1);
        else
            pairs = syma_cat(pairs, syma_one(pair));
    }
    if (syma_headis(request, "StorageGet")) {
        free(path);
        return reply("StorageGetComplete", id,
                     found ? syma_named("Found", syma_one(found)) : syma_sym("Missing"));
    }
    if (syma_headis(request, "StorageSet"))
        pairs =
            syma_cat(pairs, syma_one(syma_named("Pair", syma_cat(syma_one(key), syma_one(value)))));
    text = syma_show(syma_named("Store", pairs), 0);
    f = fopen(path, "w");
    free(path);
    if (!f) {
        free(text);
        return reply(syma_headis(request, "StorageSet") ? "StorageSetComplete"
                                                        : "StorageDelComplete",
                     id, ioerror());
    }
    int ok = fputs(text, f) >= 0;
    ok = fclose(f) == 0 && ok;
    free(text);
    return reply(syma_headis(request, "StorageSet") ? "StorageSetComplete" : "StorageDelComplete",
                 id, ok ? syma_sym("Ok") : ioerror());
}
Id syma_run_effects(Engine *e, Id program, unsigned *exited) {
    Id eff, pending, req, id, response = 0, v;
    Count i;
    unsigned wants_input;
    double now, nearest;
    Timer *t, **pt;
    char *path, *text;
    FILE *f;
    int c, ready;
    fd_set rd;
    struct timeval timeout;
    (void)e;
    for (;;) {
        eff = syma_section(program, "Effects");
        pending = syma_section(eff, "Pending");
        if (!pending || !syma_argcnt(pending)) {
            *exited = 1;
            syma_effects_cleanup();
            return program;
        }
        pt = &timers;
        while ((t = *pt)) {
            if (!pending_contains(pending, t->request)) {
                *pt = t->next;
                free(t);
            } else
                pt = &t->next;
        }
        if (input_request && !pending_contains(pending, input_request)) {
            console_input.n = 0;
            input_request = 0;
            if (terminal_raw) {
                tcsetattr(0, TCSANOW, &saved_terminal);
                terminal_raw = 0;
            }
        }
        wants_input = 0;
        nearest = -1;
        now = realtime();
        ready = 0;
        for (i = 0; i < syma_argcnt(pending); i++) {
            req = syma_arg(pending, i);
            if (!req || syma_node(req)->kind != CALL || !syma_node(req)->h ||
                syma_node(syma_node(req)->h)->kind != SYM || !syma_argcnt(req))
                syma_fail("malformed effect request");
            id = syma_arg(req, 0);
            response = 0;
            if (syma_headis(req, "Exit")) {
                *exited = 1;
                process_status = id && syma_node(id)->kind == NUM ? (int)syma_node(id)->num : 0;
                syma_effects_cleanup();
                return program;
            }
            if (syma_headis(req, "Timer") || syma_headis(req, "AnimationFrame")) {
                for (t = timers; t && t->request != req; t = t->next) {
                }
                if (!t) {
                    t = syma_mem(sizeof(Timer));
                    t->request = req;
                    v = syma_arg(syma_arg(req, 1), 0);
                    t->due = now + (syma_headis(req, "AnimationFrame") ? 16
                                    : v && syma_node(v)->kind == NUM   ? syma_node(v)->num
                                                                       : 0);
                    t->next = timers;
                    timers = t;
                }
                if (t->due > now) {
                    if (nearest < 0 || t->due < nearest)
                        nearest = t->due;
                    continue;
                }
                response =
                    reply(syma_headis(req, "Timer") ? "TimerComplete" : "AnimationFrameComplete",
                          id, syma_named("Now", syma_one(syma_number(now))));
                pt = &timers;
                while (*pt != t)
                    pt = &(*pt)->next;
                *pt = t->next;
                free(t);
            } else if (syma_headis(req, "Print")) {
                v = syma_arg(syma_arg(req, 1), 0);
                text = v && (syma_node(v)->kind == STR || syma_node(v)->kind == SYM) ? syma_utf8(v)
                       : v ? syma_show(v, 0)
                           : NULL;
                if (text) {
                    if (v && (syma_node(v)->kind == STR || syma_node(v)->kind == SYM))
                        textwrite(stdout, v);
                    else
                        fputs(text, stdout);
                    free(text);
                }
                fflush(stdout);
                response = reply("PrintComplete", id, syma_sym("Success"));
            } else if (syma_headis(req, "RandRequest")) {
                double min = syma_arg(req, 1) && syma_node(syma_arg(req, 1))->kind == NUM
                                 ? syma_node(syma_arg(req, 1))->num
                                 : 0,
                       max = syma_arg(req, 2) && syma_node(syma_arg(req, 2))->kind == NUM
                                 ? syma_node(syma_arg(req, 2))->num
                                 : 1;
                response = reply("RandResponse", id,
                                 syma_number(floor(min + (double)rand() / ((double)RAND_MAX + 1) *
                                                             (max - min + 1))));
            } else if (syma_headis(req, "ReadLine") || syma_headis(req, "GetChar")) {
                wants_input = 1;
                if (input_request && input_request != req)
                    continue;
                if (!input_request) {
                    input_request = req;
                    console_input.n = 0;
                }
                if (syma_headis(req, "GetChar") && !terminal_raw && isatty(0) &&
                    tcgetattr(0, &saved_terminal) == 0) {
                    struct termios raw = saved_terminal;
                    raw.c_lflag &= ~(ICANON | ECHO);
                    raw.c_cc[VMIN] = 1;
                    raw.c_cc[VTIME] = 0;
                    if (!tcsetattr(0, TCSANOW, &raw))
                        terminal_raw = 1;
                }
                FD_ZERO(&rd);
                FD_SET(0, &rd);
                timeout.tv_sec = timeout.tv_usec = 0;
                ready = select(1, &rd, NULL, NULL, &timeout);
                if (ready <= 0)
                    continue;
                char ch;
                int got = (int)read(0, &ch, 1);
                if (got < 0) {
                    if (errno == EINTR)
                        continue;
                    response =
                        reply(syma_headis(req, "ReadLine") ? "ReadLineComplete" : "GetCharComplete",
                              id, ioerror());
                } else {
                    if (got > 0 && (syma_headis(req, "GetChar") || (ch != 10 && ch != 13))) {
                        syma_bufput(&console_input, (unsigned char)ch);
                        if (syma_headis(req, "ReadLine"))
                            continue;
                        unsigned first = (unsigned char)console_input.p[0];
                        Count need = first < 0xc0 ? 1 : first < 0xe0 ? 2 : first < 0xf0 ? 3 : 4;
                        if (console_input.n < need)
                            continue;
                    }
                    response = reply(
                        syma_headis(req, "ReadLine") ? "ReadLineComplete" : "GetCharComplete", id,
                        syma_named(
                            syma_headis(req, "ReadLine") ? "Text" : "Char",
                            syma_one(syma_utf8atom(STR, console_input.p ? console_input.p : "",
                                                   console_input.n))));
                }
                input_request = 0;
                console_input.n = 0;
                if (terminal_raw) {
                    tcsetattr(0, TCSANOW, &saved_terminal);
                    terminal_raw = 0;
                }
            } else if (syma_headis(req, "FileRead") || syma_headis(req, "ReadSymaFile")) {
                v = syma_arg(syma_arg(req, 1), 0);
                if (!v || syma_node(v)->kind != STR)
                    syma_fail("file effect requires Path(string)");
                path = syma_utf8(v);
                f = fopen(path, "r");
                free(path);
                if (!f)
                    response = reply(syma_headis(req, "FileRead") ? "FileReadComplete"
                                                                  : "ReadSymaFileComplete",
                                     id, ioerror());
                else {
                    Buffer data = {0};
                    while ((c = fgetc(f)) != EOF)
                        syma_bufput(&data, (unsigned)c);
                    fclose(f);
                    v = syma_headis(req, "FileRead")
                            ? syma_named("Content",
                                         syma_one(syma_utf8atom(STR, data.p ? data.p : "", data.n)))
                            : syma_named("Inert", syma_one(syma_parse(data.p ? data.p : "")));
                    free(data.p);
                    response = reply(syma_headis(req, "FileRead") ? "FileReadComplete"
                                                                  : "ReadSymaFileComplete",
                                     id, v);
                }
            } else if (syma_headis(req, "FileWrite") || syma_headis(req, "WriteSymaFile")) {
                v = syma_arg(syma_arg(req, 1), 0);
                if (!v || syma_node(v)->kind != STR)
                    syma_fail("file effect requires Path(string)");
                path = syma_utf8(v);
                f = fopen(path, "w");
                free(path);
                if (!f)
                    response = reply(syma_headis(req, "FileWrite") ? "FileWriteComplete"
                                                                   : "WriteSymaFileComplete",
                                     id, ioerror());
                else {
                    text = syma_headis(req, "FileWrite")
                               ? syma_utf8(syma_arg(syma_arg(req, 2), 0))
                               : syma_show(syma_arg(syma_arg(req, 2), 0), 0);
                    int ok = syma_headis(req, "FileWrite")
                                 ? textwrite(f, syma_arg(syma_arg(req, 2), 0))
                                 : fputs(text, f) >= 0;
                    ok = fclose(f) == 0 && ok;
                    free(text);
                    response = reply(syma_headis(req, "FileWrite") ? "FileWriteComplete"
                                                                   : "WriteSymaFileComplete",
                                     id, ok ? syma_sym("Ok") : ioerror());
                }
            } else if (syma_headis(req, "StorageGet") || syma_headis(req, "StorageSet") ||
                       syma_headis(req, "StorageDel"))
                response = storage(req);
            else {
                text = syma_utf8(syma_node(req)->h);
                path = syma_mem(((Count)strlen(text) + 9) * sizeof(char));
                strcpy(path, text);
                strcat(path, "Complete");
                response = reply(
                    path, id,
                    syma_named("Error", syma_one(syma_str("effect unavailable on this platform"))));
                free(text);
                free(path);
            }
            if (response) {
                program = remove_effect(program, req, response);
                *exited = 0;
                return program;
            }
        }
        /* No synchronous effect is ready. Wait for input or the earliest timer. */
        FD_ZERO(&rd);
        if (wants_input)
            FD_SET(0, &rd);
        if (nearest >= 0) {
            double delay = fmax(0, nearest - realtime());
            timeout.tv_sec = (long)(delay / 1000);
            timeout.tv_usec = (long)(fmod(delay, 1000) * 1000);
        }
        ready = select(wants_input ? 1 : 0, wants_input ? &rd : NULL, NULL, NULL,
                       nearest >= 0 ? &timeout : NULL);
        if (ready < 0 && errno != EINTR)
            syma_fail("effect wait failed");
    }
}

double syma_random(void) {
    char *fixed = getenv("SYMA_TEST_RANDOM");
    return fixed ? strtod(fixed, NULL) : (double)rand() / ((double)RAND_MAX + 1);
}

void syma_mark_effects(void (*mark)(Id)) {
    Timer *t;
    for (t = timers; t; t = t->next)
        mark(t->request);
    mark(input_request);
}
