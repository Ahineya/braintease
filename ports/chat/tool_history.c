/* SPDX-License-Identifier: MIT */
/* Calls are saved before effects. Missing receipts are never re-executed. */
#include "chat.h"
#include "json.h"
#include <stdlib.h>
#include <string.h>

#define TOOL_HISTORY 512
typedef struct { char *id, *name, *arguments; int done; } Call;
int chat_tools_pending(Chat *c, int execute, int fd) {
    Call *calls = calloc(TOOL_HISTORY, sizeof(*calls));
    char path[320], *raw = NULL, *id, *output = NULL, *record = NULL;
    unsigned count = 0, i, at;
    int n = 0, processed = 0, type, ok = 0;
    unsigned long pending_size = 0;
    FILE *history = NULL, *body = NULL;
    CJ j;
    if (!calls) goto done;
    chat_history_path(c, path);
    history = fopen(path, "r");
    if (!history) goto done;
    while ((n = chat_object(history, &raw)) > 0) {
        if (!cj_parse(&j, raw, strlen(raw))) goto done;
        type = cj_field(&j, 0, "type");
        if (cj_equal(&j, type, "function_call") || cj_equal(&j, type, "function_call_output")) {
            id = cj_string(&j, cj_field(&j, 0, "call_id"));
            if (!id || !*id || strlen(id) > 128) { free(id); cj_free(&j); goto done; }
            for (at = 0; at < count; at++) if (!strcmp(id, calls[at].id)) break;
            if (cj_equal(&j, type, "function_call")) {
                if (at != count || count == TOOL_HISTORY) { free(id); cj_free(&j); goto done; }
                calls[count].id = id;
                calls[count].name = cj_string(&j, cj_field(&j, 0, "name"));
                calls[count].arguments = cj_string(&j, cj_field(&j, 0, "arguments"));
                ++count;
                if (!calls[at].name || !*calls[at].name || strlen(calls[at].name) > 64 ||
                    !calls[at].arguments || strlen(calls[at].arguments) > 131072UL) { cj_free(&j); goto done; }
                pending_size += strlen(calls[at].arguments);
                if (pending_size > CHAT_ITEM) { cj_free(&j); goto done; }
            } else {
                free(id);
                if (at == count || calls[at].done) { cj_free(&j); goto done; }
                pending_size -= strlen(calls[at].arguments);
                free(calls[at].arguments); calls[at].arguments = NULL;
                free(calls[at].name); calls[at].name = NULL;
                calls[at].done = 1;
            }
        }
        cj_free(&j);
        free(raw); raw = NULL;
    }
    fclose(history); history = NULL;
    if (n < 0) goto done;
    for (i = 0; i < count; i++) {
        if (calls[i].done) continue;
        if (execute > 0 && !chat_tools_cancelled()) {
            char status[128];
            snprintf(status, sizeof(status), "Running tool: %s | Esc stops", calls[i].name);
            chat_message(fd, 'S', status);
            output = chat_tool_run(c, calls[i].name, calls[i].arguments, fd);
        } else
            output = strdup("{\"ok\":false,\"interrupted\":true,\"text\":\"Tool interrupted before a durable result. It may have executed. Inspect files/processes before repeating effects; it was not automatically re-executed.\"}");
        if (!output || !(body = chat_temporary(c))) goto done;
        fputs("{\"type\":\"function_call_output\",\"call_id\":", body);
        if (!cj_quote(body, calls[i].id)) goto done;
        fputs(",\"output\":", body);
        if (!cj_quote(body, output) || fputs("}\n", body) == EOF ||
            !(record = chat_file_text(body, CHAT_ITEM)) || !chat_commit(c, record)) goto done;
        fclose(body); body = NULL;
        free(record); record = NULL;
        free(output); output = NULL;
        ++processed;
        if (execute >= 0) chat_message(fd, 'H', "");
    }
    ok = 1;
done:
    if (history) fclose(history);
    if (body) fclose(body);
    free(output); free(record); free(raw);
    if (calls) for (i = 0; i < count; i++) {
        free(calls[i].id); free(calls[i].name); free(calls[i].arguments);
    }
    free(calls);
    if (!ok) {
        strcpy(c->status, "Cannot read/record tool history (invalid IDs, limit or storage error)");
        return -1;
    }
    return processed;
}
