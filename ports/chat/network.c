/* SPDX-License-Identifier: MIT */
#include "chat.h"
#include "http.h"
#include "json.h"
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

typedef struct {
    char *data;
    unsigned long used, limit, capacity;
} Buffer;
static const char *setting(const char *name, const char *fallback) {
    const char *s = getenv(name);
    return s && *s ? s : fallback;
}
static int collect(void *context, const char *data, unsigned n) {
    Buffer *b = context;
    char *p;
    if (n > b->limit - b->used)
        return -1;
    if (b->used + n + 1 > b->capacity) {
        unsigned long cap = b->capacity ? b->capacity : 2048;
        while (cap < b->used + n + 1)
            cap *= 2;
        if (cap > b->limit + 1)
            cap = b->limit + 1;
        p = realloc(b->data, cap);
        if (!p)
            return -1;
        b->data = p;
        b->capacity = cap;
    }
    p = b->data;
    memcpy(p + b->used, data, n);
    b->used += n;
    p[b->used] = 0;
    return 0;
}
static int collect_span(Buffer *b, const char *p, unsigned long n) {
    while (n) {
        unsigned take = n > 4096 ? 4096 : (unsigned)n;
        if (collect(b, p, take))
            return -1;
        p += take;
        n -= take;
    }
    return 0;
}
static int write_all(int fd, const char *data, unsigned n) {
    ssize_t done;
    while (n) {
        done = write(fd, data, n);
        if (done < 0 && errno == EINTR)
            continue;
        if (done <= 0)
            return 0;
        data += done;
        n -= done;
    }
    return 1;
}
void chat_message(int fd, unsigned kind, const char *data) {
    unsigned long size = strlen(data);
    unsigned take;
    char head[3];
    do {
        take = size > 4096 ? 4096 : (unsigned)size;
        head[0] = kind;
        head[1] = take >> 8;
        head[2] = take & 255;
        if (!write_all(fd, head, 3) || !write_all(fd, data, take))
            _exit(1);
        data += take;
        size -= take;
    } while (size);
}
static void form(FILE *f, const char *text) {
    unsigned c;
    while ((c = (unsigned char)*text++) != 0) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
            strchr("-._~", c))
            fputc(c, f);
        else
            fprintf(f, "%%%02X", c);
    }
}
/* Refresh in the worker while the parent's history-directory lock stays held.
 * Preserve registration/identity and replace rotating credentials atomically. */
static int refresh(Chat *c, char **account, char *error) {
    CJ old, next;
    char *cid = NULL, *token = NULL, *type = NULL, *new_access = NULL, *new_refresh = NULL,
         *joined = NULL, path[320];
    FILE *body = NULL, *result = NULL;
    Buffer response = {NULL, 0, 65536UL, 0};
    int status, ok = 0, scopes;
    unsigned i;
    unsigned long seconds;
    if (!cj_parse(&old, *account, strlen(*account)))
        return 0;
    cid = cj_string(&old, cj_field(&old, 0, "client_id"));
    token = cj_string(&old, cj_field(&old, 0, "refresh_token"));
    if (!cid || !token) {
        strcpy(error, "Missing refresh credentials; sign in again");
        goto done;
    }
    body = chat_temporary(c);
    if (!body)
        goto done;
    fputs("grant_type=refresh_token&client_id=", body);
    form(body, cid);
    fputs("&refresh_token=", body);
    form(body, token);
    fputs("&resource=https%3A%2F%2Fapi.openai.com%2Fv1", body);
    fflush(body);
    status = chat_http(setting("CHAT_AUTH_ORIGIN", "https://auth.openai.com"),
                       "/api/accounts/oauth/token", NULL, "application/x-www-form-urlencoded", body,
                       setting("CHAT_CA", "/etc/ssl/cert.pem"), collect, &response, error, 256);
    if (status != 200)
        goto done;
    if (!response.data || !cj_parse(&next, response.data, response.used)) {
        strcpy(error, "Invalid refresh response");
        goto done;
    }
    new_access = cj_string(&next, cj_field(&next, 0, "access_token"));
    new_refresh = cj_string(&next, cj_field(&next, 0, "refresh_token"));
    type = cj_string(&next, cj_field(&next, 0, "token_type"));
    if (!new_access || !*new_access || !new_refresh || !*new_refresh || !type ||
        strcasecmp(type, "Bearer") ||
        !cj_number(&next, cj_field(&next, 0, "expires_in"), &seconds) || !seconds ||
        seconds > 604800UL) {
        strcpy(error, "Incomplete refresh response; sign in again");
        cj_free(&next);
        goto done;
    }
    scopes = cj_field(&next, 0, "scope");
    if (scopes >= 0) {
        char *granted = cj_string(&next, scopes);
        char *p;
        int direct = 0, invoke = 0;
        if (granted) {
            p = strtok(granted, " ");
            while (p) {
                if (!strcmp(p, "chatgpt.tokens.use.direct"))
                    direct = 1;
                if (!strcmp(p, "resource.invoke"))
                    invoke = 1;
                p = strtok(NULL, " ");
            }
        }
        free(granted);
        if (!direct || !invoke) {
            strcpy(error, "Refresh no longer grants direct inference; sign in again");
            cj_free(&next);
            goto done;
        }
    }
    result = chat_temporary(c);
    if (!result) {
        cj_free(&next);
        goto done;
    }
    fputc('{', result);
    for (i = 1; i < old.tokens[0].after; i = old.tokens[i + 1].after) {
        if (cj_equal(&old, i, "access_token") || cj_equal(&old, i, "refresh_token") ||
            cj_equal(&old, i, "expires_at") || cj_equal(&old, i, "expires_in") ||
            cj_equal(&old, i, "saved_at") || (scopes >= 0 && cj_equal(&old, i, "scopes")))
            continue;
        fwrite(old.text + old.tokens[i].start, 1, old.tokens[i].end - old.tokens[i].start, result);
        fputc(':', result);
        fwrite(old.text + old.tokens[i + 1].start, 1,
               old.tokens[i + 1].end - old.tokens[i + 1].start, result);
        fputc(',', result);
    }
    if (scopes >= 0) {
        char *granted = cj_string(&next, scopes), *p;
        int first = 1;
        fputs("\"scopes\":[", result);
        p = strtok(granted, " ");
        while (p) {
            if (!first)
                fputc(',', result);
            cj_quote(result, p);
            first = 0;
            p = strtok(NULL, " ");
        }
        fputs("],", result);
        free(granted);
    }
    fputs("\"access_token\":", result);
    cj_quote(result, new_access);
    fputs(",\"refresh_token\":", result);
    cj_quote(result, new_refresh);
    fprintf(result, ",\"expires_at\":%lu,\"expires_in\":%lu}", (unsigned long)time(NULL) + seconds,
            seconds);
    if (fflush(result) || ferror(result)) {
        cj_free(&next);
        goto done;
    }
    {
        long n = ftell(result);
        joined = malloc((unsigned long)n + 1);
        if (!joined || fseek(result, 0, SEEK_SET) ||
            fread(joined, 1, n, result) != (unsigned long)n) {
            cj_free(&next);
            goto done;
        }
        joined[n] = 0;
    }
    chat_path(c, path, "account.json");
    ok = chat_atomic(path, joined);
    cj_free(&next);
    if (ok) {
        free(*account);
        *account = joined;
        joined = NULL;
    } else
        strcpy(error, "Cannot save refreshed credentials");
done:
    if (body)
        fclose(body);
    if (result)
        fclose(result);
    free(cid);
    free(token);
    free(type);
    free(new_access);
    free(new_refresh);
    free(joined);
    free(response.data);
    cj_free(&old);
    if (!ok && !*error)
        strcpy(error, "Could not refresh credentials");
    return ok;
}

typedef struct {
    Buffer line, event, text, finished;
    int fd, done, cr, finished_text, finished_calls, calls, synthesize;
    char error[256];
    char *items;
    unsigned long text_size;
} Stream;
/* Retain individually completed items as well as the response snapshot,
 * including encrypted reasoning needed for replay. */
static int item_text(CJ *j, unsigned item) {
    int content = cj_field(j, item, "content"), text;
    unsigned i;
    char *s;
    if (!cj_equal(j, cj_field(j, item, "role"), "assistant") || content < 0)
        return 0;
    if (j->tokens[content].kind == '"') {
        s = cj_string(j, content);
        text = s && *s;
        free(s);
        return text;
    }
    if (j->tokens[content].kind != '[')
        return 0;
    for (i = content + 1; i < j->tokens[content].after; i = j->tokens[i].after) {
        text = cj_field(j, i, "text");
        if (text < 0)
            text = cj_field(j, i, "refusal");
        s = cj_string(j, text);
        text = s && *s;
        free(s);
        if (text)
            return 1;
    }
    return 0;
}
static int valid_call(CJ *j, unsigned item) {
    char *id = cj_string(j, cj_field(j, item, "call_id"));
    char *name = cj_string(j, cj_field(j, item, "name"));
    char *arguments = cj_string(j, cj_field(j, item, "arguments"));
    int ok = id && *id && strlen(id) <= 128 && name && *name && strlen(name) <= 64 &&
             arguments && strlen(arguments) <= 131072UL;
    free(id); free(name); free(arguments);
    return ok;
}
static int event(Stream *s) {
    CJ j;
    int type, response, output, status, visible = 0, calls = 0;
    unsigned i;
    char *text;
    Buffer items = {NULL, 0, CHAT_ITEM, 0};
    if (!s->event.used)
        return 0;
    if (s->done && s->event.used == 6 && !memcmp(s->event.data, "[DONE]", 6)) {
        s->event.used = 0;
        s->event.data[0] = 0;
        return 0;
    }
    if (s->done) {
        strcpy(s->error, "Unexpected event after completed response");
        return -1;
    }
    if (!cj_parse(&j, s->event.data, s->event.used)) {
        strcpy(s->error, "Invalid streaming JSON");
        return -1;
    }
    type = cj_field(&j, 0, "type");
    if (cj_equal(&j, type, "response.output_text.delta") ||
        cj_equal(&j, type, "response.refusal.delta")) {
        text = cj_string(&j, cj_field(&j, 0, "delta"));
        if (!text) {
            strcpy(s->error, "Invalid text delta");
            goto bad;
        }
        s->text_size += strlen(text);
        if (s->text_size >= CHAT_TEXT) {
            free(text);
            strcpy(s->error, "Reply exceeds the 128 KiB display limit");
            goto bad;
        }
        if (collect_span(&s->text, text, strlen(text))) {
            free(text);
            strcpy(s->error, "Cannot retain streamed reply");
            goto bad;
        }
        chat_message(s->fd, 'T', text);
        free(text);
    } else if (cj_equal(&j, type, "response.output_item.done")) {
        output = cj_field(&j, 0, "item");
        if (output < 0 || j.tokens[output].kind != '{' ||
            collect_span(&s->finished, j.text + j.tokens[output].start,
                         j.tokens[output].end - j.tokens[output].start) ||
            collect(&s->finished, "\n", 1)) {
            strcpy(s->error, "Invalid or oversized completed output item");
            goto bad;
        }
        if (item_text(&j, output))
            s->finished_text = 1;
        if (cj_equal(&j, cj_field(&j, output, "type"), "function_call")) {
            if (!valid_call(&j, output)) { strcpy(s->error, "Invalid function call fields"); goto bad; }
            s->finished_calls = 1;
        }
    } else if (cj_equal(&j, type, "response.completed")) {
        response = cj_field(&j, 0, "response");
        status = cj_field(&j, response, "status");
        output = cj_field(&j, response, "output");
        if (!cj_equal(&j, status, "completed") || output < 0 || j.tokens[output].kind != '[') {
            strcpy(s->error, "Incomplete response payload");
            goto bad;
        }
        for (i = output + 1; i < j.tokens[output].after; i = j.tokens[i].after) {
            if (cj_equal(&j, cj_field(&j, i, "type"), "function_call") && !valid_call(&j, i)) {
                free(items.data);
                strcpy(s->error, "Invalid function call fields"); goto bad;
            }
            if (j.tokens[i].kind != '{' ||
                collect_span(&items, j.text + j.tokens[i].start,
                             j.tokens[i].end - j.tokens[i].start) ||
                collect(&items, "\n", 1)) {
                free(items.data);
                strcpy(s->error, "Completed response exceeds history item limit");
                goto bad;
            }
            if (item_text(&j, i))
                visible = 1;
            if (cj_equal(&j, cj_field(&j, i, "type"), "function_call"))
                calls = 1;
        }
        if ((!items.used || (!visible && !calls)) && s->finished.used &&
            (!items.used || s->finished_text || s->finished_calls)) {
            free(items.data);
            items = s->finished;
            memset(&s->finished, 0, sizeof(s->finished));
            visible = s->finished_text;
            calls = s->finished_calls;
        }
        s->items = items.data ? items.data : strdup("");
        if (!s->items) {
            strcpy(s->error, "Cannot retain completed reply");
            goto bad;
        }
        if (!visible && !s->text.used && !calls) {
            strcpy(s->error, "Service completed without a reply. Ctrl-R retries your saved message.");
            goto bad;
        }
        /* Never discard text the user has seen just because the final output
         * snapshot is empty. Only persist it after confirmed completion. */
        s->synthesize = !visible && s->text.used;
        s->calls = calls;
        s->done = 1;
    } else if (cj_equal(&j, type, "response.failed") || cj_equal(&j, type, "response.incomplete") ||
               cj_equal(&j, type, "error")) {
        strcpy(
            s->error,
            "The service did not complete this reply. Your message is saved; use Ctrl-R to retry.");
        goto bad;
    }
    cj_free(&j);
    s->event.used = 0;
    if (s->event.data)
        s->event.data[0] = 0;
    return 0;
bad:
    cj_free(&j);
    return -1;
}
static int stream_line(Stream *s) {
    char *p = s->line.data;
    unsigned long n = s->line.used;
    if (n && p[n - 1] == '\r')
        p[--n] = 0;
    if (!n) {
        if (event(s))
            return -1;
    } else if (n >= 5 && !memcmp(p, "data:", 5)) {
        p += 5;
        n -= 5;
        if (n && *p == ' ') {
            p++;
            n--;
        }
        if (s->event.used && collect(&s->event, "\n", 1))
            goto large;
        if (n > 65535UL) { /* C16 callback counts stay bounded. */
            while (n > 4096) {
                if (collect(&s->event, p, 4096))
                    goto large;
                p += 4096;
                n -= 4096;
            }
        }
        if (collect(&s->event, p, (unsigned)n))
            goto large;
    }
    s->line.used = 0;
    if (s->line.data)
        s->line.data[0] = 0;
    return 0;
large:
    strcpy(s->error, "Streaming event exceeds 512 KiB");
    return -1;
}
static int receive(void *context, const char *p, unsigned n) {
    if (chat_tools_cancelled()) return -1;
    Stream *s = context;
    unsigned i;
    for (i = 0; i < n; i++) {
        if (!p[i]) {
            strcpy(s->error, "NUL in SSE stream");
            return -1;
        }
        if (p[i] == '\r') {
            if (stream_line(s))
                return -1;
            s->cr = 1;
        } else if (p[i] == '\n') {
            if (!s->cr && stream_line(s))
                return -1;
            s->cr = 0;
        } else {
            s->cr = 0;
            if (collect(&s->line, p + i, 1)) {
                strcpy(s->error, "Streaming line exceeds 512 KiB");
                return -1;
            }
        }
    }
    return 0;
}
void chat_worker(Chat *c, int fd) {
    char path[320], error[256] = "", *account = NULL, *token = NULL;
    CJ j;
    FILE *body = NULL;
    Stream s;
    unsigned long expiry = 0;
    int status, refreshed = 0, rounds = 0, tools;
    memset(&s, 0, sizeof(s));
    signal(SIGPIPE, SIG_DFL);
    chat_tools_init();
    chat_path(c, path, "account.json");
    account = chat_read(path, 65536UL);
    if (!account) {
        strcpy(error, "Sign in first: chat --login-request FILE, then chat --import FILE");
        goto done;
    }
    if (!cj_parse(&j, account, strlen(account))) {
        strcpy(error, "Invalid saved credentials");
        goto done;
    }
    if (!cj_number(&j, cj_field(&j, 0, "expires_at"), &expiry)) {
        cj_free(&j);
        strcpy(error, "Missing credential expiry");
        goto done;
    }
    cj_free(&j);
    if (expiry <= (unsigned long)time(NULL) + 90) {
        chat_message(fd, 'S', "Refreshing sign-in...");
        if (!refresh(c, &account, error))
            goto done;
        refreshed = 1;
    }
retry:
    free(s.line.data); free(s.event.data); free(s.text.data); free(s.finished.data); free(s.items);
    memset(&s, 0, sizeof(s));
    s.fd = fd;
    s.line.limit = s.event.limit = CHAT_ITEM;
    s.text.limit = CHAT_TEXT - 1;
    s.finished.limit = CHAT_ITEM;
    error[0] = 0;
    if (chat_tools_cancelled()) { strcpy(error, "Cancelled"); goto done; }
    if (!cj_parse(&j, account, strlen(account)))
        goto done;
    token = cj_string(&j, cj_field(&j, 0, "access_token"));
    cj_free(&j);
    if (!token || !*token) {
        strcpy(error, "Missing access token");
        goto done;
    }
    body = chat_request(c);
    if (!body) {
        snprintf(error, sizeof(error), "%s", c->status);
        goto done;
    }
    chat_message(fd, 'S', "Connecting | GPT-6.1 Sol Light...");
    status = chat_http(setting("CHAT_API_ORIGIN", "https://api.openai.com"), "/v1/responses", token,
                       "application/json", body, setting("CHAT_CA", "/etc/ssl/cert.pem"), receive,
                       &s, error, sizeof(error));
    fclose(body);
    body = NULL;
    free(token);
    token = NULL;
    if (status == 401 && !refreshed && !s.text_size) {
        if (!refresh(c, &account, error))
            goto done;
        refreshed = 1;
        goto retry;
    }
    if (*s.error) {
        strcpy(error, s.error);
        goto done;
    }
    if (status != 200)
        goto done;
    /* EOF alone never constitutes a successful model reply. */
    if (!s.done || s.event.used || s.line.used) {
        strcpy(error, "Connection ended without a complete response; your message is saved");
        goto done;
    }
    if (s.synthesize) {
        char *saved;
        long size;
        body = chat_temporary(c);
        if (!body) {
            strcpy(error, "Cannot preserve streamed reply");
            goto done;
        }
        fputs(s.items, body);
        fputs("{\"role\":\"assistant\",\"content\":", body);
        if (!cj_quote(body, s.text.data) || fputs("}\n", body) == EOF ||
            fflush(body) || ferror(body) || (size = ftell(body)) < 0 ||
            (unsigned long)size > CHAT_ITEM || !(saved = malloc((unsigned long)size + 1))) {
            strcpy(error, "Cannot preserve streamed reply");
            goto done;
        }
        if (fseek(body, 0, SEEK_SET) || fread(saved, 1, size, body) != (unsigned long)size) {
            free(saved);
            strcpy(error, "Cannot preserve streamed reply");
            goto done;
        }
        saved[size] = 0;
        free(s.items);
        s.items = saved;
        fclose(body);
        body = NULL;
    }
    if (s.calls && !c->workspace[0]) {
        strcpy(error, "Service requested tools in a plain chat; no tools were executed");
        goto done;
    }
    if (!chat_commit(c, s.items)) {
        strcpy(error, "Cannot save completed reply; history limit or disk full");
        goto done;
    }
    chat_message(fd, 'H', "");
    if (s.calls) {
        tools = chat_tools_pending(c, 1, fd);
        if (tools <= 0) {
            snprintf(error, sizeof(error), "%s", tools < 0 ? c->status : "No pending tool calls found");
            goto done;
        }
        if (++rounds == 32) {
            strcpy(error, "Stopped after 32 tool rounds. Results saved; Ctrl-R continues.");
            goto done;
        }
        refreshed = 0; /* Each model request may refresh once during a long tool turn. */
        goto retry;
    }
    chat_message(fd, 'D', "Reply saved");
done:
    if (*error)
        chat_message(fd, 'E', error);
    else if (!s.done)
        chat_message(fd, 'E', "Request failed");
    if (body)
        fclose(body);
    free(account);
    free(token);
    free(s.line.data);
    free(s.event.data);
    free(s.text.data);
    free(s.finished.data);
    free(s.items);
    close(fd);
    _exit(*error || !s.done ? 1 : 0);
}
int chat_send(Chat *c, const char *text) {
    char *item, path[320];
    FILE *f;
    long size;
    int descriptors[2];
    pid_t pid;
    if (c->worker)
        return 0;
    if (c->count && chat_tools_pending(c, -1, 0) < 0)
        return 0;
    if (*text) {
        if (strlen(text) > CHAT_INPUT || !cj_utf8(text, strlen(text))) {
            strcpy(c->status, "Message is too long or contains invalid UTF-8");
            return 0;
        }
        if (!c->count && !chat_new(c))
            return 0;
        f = chat_temporary(c);
        if (!f)
            return 0;
        fputs("{\"role\":\"user\",\"content\":", f);
        cj_quote(f, text);
        fputs("}\n", f);
        if (fflush(f) || ferror(f) || (size = ftell(f)) < 0) {
            fclose(f);
            strcpy(c->status, "Cannot write message");
            return 0;
        }
        item = malloc(size + 1);
        if (!item || fseek(f, 0, SEEK_SET) || fread(item, 1, size, f) != (unsigned long)size) {
            free(item);
            fclose(f);
            return 0;
        }
        fclose(f);
        item[size] = 0;
        if (!chat_commit(c, item)) {
            free(item);
            strcpy(c->status, "Cannot save message; history limit or disk full");
            return 0;
        }
        free(item);
        if (!strcmp(c->titles[c->selected], "New chat")) {
            unsigned n = 0;
            while (text[n] && n < 90) {
                c->titles[c->selected][n] = (unsigned char)text[n] < 32 ? ' ' : text[n];
                ++n;
            }
            while (n && ((unsigned char)text[n] & 192) == 128)
                --n;
            c->titles[c->selected][n] = 0;
        }
    } else if (!c->count) {
        strcpy(c->status, "Write a message first");
        return 0;
    } else { /* Retry only an unanswered user turn. */
        char *history;
        CJ j;
        char *last;
        snprintf(path, sizeof(path), "%s/c%06u.jsonl", c->root, c->ids[c->selected]);
        history = chat_read(path, CHAT_HISTORY);
        if (!history)
            return 0;
        size = strlen(history);
        if (size && history[size - 1] == '\n')
            history[--size] = 0;
        last = strrchr(history, '\n');
        last = last ? last + 1 : history;
        if (!cj_parse(&j, last, strlen(last))) {
            free(history);
            return 0;
        }
        pid = cj_equal(&j, cj_field(&j, 0, "role"), "user") ||
              cj_equal(&j, cj_field(&j, 0, "type"), "function_call_output");
        cj_free(&j);
        free(history);
        if (!pid) {
            strcpy(c->status, "The last message already has a reply");
            return 0;
        }
    }
    chat_load(c);
    if (pipe(descriptors)) {
        strcpy(c->status, "Cannot create worker pipe");
        return 0;
    }
    pid = fork();
    if (pid < 0) {
        close(descriptors[0]);
        close(descriptors[1]);
        strcpy(c->status, "No process slot for chat worker");
        return 0;
    }
    if (!pid) {
        close(descriptors[0]);
        chat_worker(c, descriptors[1]);
    }
    close(descriptors[1]);
    c->pipefd = descriptors[0];
    fcntl(c->pipefd, F_SETFL, O_NONBLOCK);
    c->worker = pid;
    {
        extern int setpgid(pid_t, pid_t);
        setpgid(pid, pid);
    }
    c->framed = 0;
    c->outcome = 0;
    strcpy(c->status, "Starting request...");
    ++c->revision;
    return 1;
}
void chat_step(Chat *c) {
    int status;
    ssize_t n;
    unsigned len;
    char *payload;
    if (!c->worker)
        return;
    /* Drain at most 16 KiB per UI tick. Frames may split at any pipe boundary. */
    {
        unsigned budget = 16384;
        while (budget) {
            unsigned need =
                c->framed < 3 ? 3
                              : 3 + ((unsigned char)c->frame[1] * 256 + (unsigned char)c->frame[2]);
            if (need > 4099) {
                strcpy(c->status, "Invalid worker frame");
                chat_cancel(c);
                return;
            }
            n = read(c->pipefd, c->frame + c->framed, need - c->framed);
            if (n <= 0)
                break;
            c->framed += n;
            budget -= n > budget ? budget : (unsigned)n;
            if (c->framed < 3)
                continue;
            len = (unsigned char)c->frame[1] * 256 + (unsigned char)c->frame[2];
            if (c->framed != len + 3)
                continue;
            payload = c->frame + 3;
            payload[len] = 0;
            if (c->frame[0] == 'T') {
                if (c->echo) {
                    unsigned i;
                    for (i = 0; i < len; i++) {
                        unsigned ch = (unsigned char)payload[i];
                        fputc((ch < 32 && ch != 10 && ch != 9) || ch == 127 ? '?' : ch, stdout);
                    }
                    fflush(stdout);
                }
                if (c->reply_size + len < CHAT_TEXT) {
                    memcpy(c->reply + c->reply_size, payload, len + 1);
                    c->reply_size += len;
                }
                strcpy(c->status, "Receiving reply... Esc cancels");
            } else if (c->frame[0] == 'H') {
                chat_load(c);
            } else if (c->frame[0] == 'E' || c->frame[0] == 'S' || c->frame[0] == 'D') {
                snprintf(c->status, sizeof(c->status), "%s", payload);
                if (c->frame[0] == 'E')
                    c->outcome = -1;
                if (c->frame[0] == 'D')
                    c->outcome = 1;
                if (c->echo && c->frame[0] == 'S') {
                    unsigned i;
                    fputc('\n', stderr);
                    for (i = 0; i < len; i++) {
                        unsigned ch = (unsigned char)payload[i];
                        fputc(ch < 32 || ch == 127 ? ' ' : ch, stderr);
                    }
                    fputc('\n', stderr);
                }
            }
            c->framed = 0;
            ++c->revision;
        }
    }
    /* Reap only after EOF: the completed message may still be buffered. */
    if (n == 0 && waitpid(c->worker, &status, WNOHANG) == c->worker) {
        close(c->pipefd);
        c->pipefd = -1;
        c->worker = 0;
        if (WIFEXITED(status) && WEXITSTATUS(status) == 0 && c->outcome == 1)
            chat_load(c);
        else if (c->outcome != -1)
            strcpy(c->status, "Reply failed. Ctrl-R retries your saved message.");
        ++c->revision;
    }
}
void chat_cancel(Chat *c) {
    int status, i;
    if (c->worker) {
        kill(c->worker, SIGTERM);
        for (i = 0; i < 50; i++) {
            if (waitpid(c->worker, &status, WNOHANG) == c->worker) break;
            usleep(10000UL);
        }
        kill(-c->worker, SIGKILL);
        if (i == 50) {
            kill(c->worker, SIGKILL);
            while (waitpid(c->worker, &status, 0) < 0 && errno == EINTR) {}
        }
        close(c->pipefd);
        c->worker = 0;
        c->pipefd = -1;
        chat_load(c);
        strcpy(c->status, c->workspace[0] ? "Cancelled; saved tool results remain. Ctrl-R continues."
                                        : "Cancelled; your message is saved. Ctrl-R retries.");
        ++c->revision;
    }
}
void chat_close(Chat *c) {
    char path[320];
    if (c->worker)
        chat_cancel(c);
    if (c->lock >= 0) {
        close(c->lock);
        chat_path(c, path, "lock");
        unlink(path);
        c->lock = -1;
    }
    free(c->display);
    free(c->reply);
    c->display = c->reply = NULL;
}
static int ignore(void *context, const char *p, unsigned n) {
    (void)context;
    (void)p;
    (void)n;
    return 0;
}
int chat_probe(Chat *c) {
    int status = chat_http(setting("CHAT_API_ORIGIN", "https://api.openai.com"), "/v1/models", NULL,
                           NULL, NULL, setting("CHAT_CA", "/etc/ssl/cert.pem"), ignore, NULL,
                           c->status, sizeof(c->status));
    if (status == 401) {
        strcpy(c->status, "HTTPS_OK: guest DNS, TCP, certificate verification and OpenAI HTTPS "
                          "work (unauthenticated HTTP 401)");
        return 1;
    }
    return 0;
}
