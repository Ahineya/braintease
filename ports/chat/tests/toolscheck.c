/* SPDX-License-Identifier: MIT */
#include "../chat.h"
#include "../json.h"
#include <assert.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

void chat_message(int fd, unsigned type, const char *text) { (void)fd; (void)type; (void)text; }
void chat_close(Chat *c) { (void)c; }
static int truth(CJ *j, const char *field) {
    int at = cj_field(j, 0, field);
    return at >= 0 && j->tokens[at].kind == 't';
}
static char *run(Chat *c, const char *name, const char *args, int ok) {
    char *raw = chat_tool_run(c, name, args, -1), *text;
    CJ j;
    assert(raw && cj_parse(&j, raw, strlen(raw)));
    assert(truth(&j, "ok") == ok);
    text = cj_string(&j, cj_field(&j, 0, "text"));
    assert(text);
    cj_free(&j); free(raw);
    return text;
}
static char *instructions(Chat *c) {
    FILE *f = chat_request(c);
    CJ j;
    char *raw, *text;
    assert(f);
    raw = chat_file_text(f, CHAT_ITEM); fclose(f);
    assert(raw && cj_parse(&j, raw, strlen(raw)));
    text = cj_string(&j, cj_field(&j, 0, "instructions"));
    assert(text);
    assert(cj_field(&j, 0, "tools") >= 0 || !c->workspace[0]);
    cj_free(&j); free(raw);
    return text;
}
int main(int argc, char **argv) {
    Chat c = {0};
    char *s, path[320], content[512], *raw;
    struct stat st;
    CJ j;
    unsigned long n;
    FILE *f;
    pid_t pid;
    int status;
    assert(argc == 3);
    strcpy(c.root, argv[1]); strcpy(c.workspace, argv[2]);
    c.lock = -1; c.count = 1; c.ids[0] = 1;
    c.display = calloc(CHAT_TEXT, 1); c.reply = calloc(CHAT_TEXT, 1);
    assert(c.display && c.reply);
    chat_history_path(&c, path); assert(chat_atomic(path, ""));
    snprintf(content, sizeof(content), "%s/profile", c.workspace);
    assert(chat_atomic(content, "Call me Pavlo. Keep Ж🙂 and \\\"quotes\\\".\n"));
    assert(chat_instructions(&c, content));
    chat_path(&c, path, ".instructions");
    assert(!stat(path, &st) && (st.st_mode & 0777) == 0600);
    s = instructions(&c);
    assert(strstr(s, "guest workspace") && strstr(s, "Call me Pavlo.") && strstr(s, "Ж🙂")); free(s);
    c.workspace[0] = 0;
    s = instructions(&c); assert(strstr(s, "Call me Pavlo.") && !strstr(s, "guest workspace")); free(s);
    strcpy(c.workspace, argv[2]);
    assert(chat_atomic(content, "Updated preferences\n"));
    assert(chat_instructions(&c, content));
    s = instructions(&c); assert(strstr(s, "Updated preferences") && !strstr(s, "Call me Pavlo.")); free(s);
    f = fopen(content, "w"); assert(f); fputc(255, f); fclose(f);
    assert(!chat_instructions(&c, content));
    s = instructions(&c); assert(strstr(s, "Updated preferences")); free(s);
    f = fopen(content, "w"); assert(f); fputs("bad", f); fputc(0, f); fclose(f);
    assert(!chat_instructions(&c, content));
    f = fopen(content, "w"); assert(f);
    for (n = 0; n <= CHAT_INSTRUCTIONS; n++) fputc('x', f);
    fclose(f); assert(!chat_instructions(&c, content));
    assert(chat_atomic(path, "\xff"));
    assert(!chat_request(&c) && strstr(c.status, ".instructions"));
    assert(chat_atomic(path, ""));
    s = instructions(&c); assert(!strstr(s, "User-provided custom instructions")); free(s);
    assert(!unlink(path));
    s = instructions(&c); assert(!strstr(s, "User-provided custom instructions")); free(s);
    assert(!symlink("missing-instructions", path));
    assert(!chat_request(&c) && strstr(c.status, ".instructions"));
    assert(!unlink(path));
    assert(chat_sidebar(&c, 1) && c.sidebar == CHAT_SIDEBAR_HIDDEN);
    chat_path(&c, path, ".sidebar"); s = chat_read(path, 16);
    assert(s && !strcmp(s, "hidden\n")); free(s);
    assert(chat_sidebar(&c, 0) && c.sidebar == CHAT_SIDEBAR_VISIBLE);
    s = chat_read(path, 16); assert(s && !strcmp(s, "visible\n")); free(s);
    chat_history_path(&c, path);
    chat_tools_init();
    s = run(&c, "write_file", "{\"path\":\"code.c\",\"content\":\"old value Ж🙂\\n\"}", 1); free(s);
    s = run(&c, "read_file", "{\"path\":\"code.c\",\"offset\":0,\"max_bytes\":32768}", 1);
    assert(!strcmp(s, "old value Ж🙂\n")); free(s);
    raw = chat_tool_run(&c, "read_file", "{\"path\":\"code.c\",\"offset\":0,\"max_bytes\":12}", -1);
    assert(cj_parse(&j, raw, strlen(raw)) && truth(&j, "ok") && truth(&j, "truncated"));
    assert(cj_number(&j, cj_field(&j, 0, "next_offset"), &n) && n == 12);
    cj_free(&j); free(raw);
    snprintf(content, sizeof(content), "%s/code.c", c.workspace);
    assert(!chmod(content, 0700));
    s = run(&c, "edit_file", "{\"path\":\"code.c\",\"old_text\":\"old\",\"new_text\":\"new\"}", 1); free(s);
    assert(!stat(content, &st) && (st.st_mode & 0777) == 0700);
    s = run(&c, "edit_file", "{\"path\":\"code.c\",\"old_text\":\"missing\",\"new_text\":\"BAD\"}", 0); free(s);
    s = run(&c, "list_directory", "{\"path\":\".\"}", 1); assert(strstr(s, "code.c")); free(s);
    snprintf(content, sizeof(content), "%s/link", c.workspace); assert(!symlink("code.c", content));
    s = run(&c, "write_file", "{\"path\":\"link\",\"content\":\"BAD\"}", 0); free(s);
    s = run(&c, "write_file", "{\"path\":\"repeat\",\"content\":\"aaa\"}", 1); free(s);
    s = run(&c, "edit_file", "{\"path\":\"repeat\",\"old_text\":\"aa\",\"new_text\":\"BAD\"}", 0); free(s);
    s = run(&c, "read_file", "{\"path\":\"repeat\",\"offset\":0,\"max_bytes\":32768}", 1); assert(!strcmp(s, "aaa")); free(s);
    s = run(&c, "run_command", "{\"command\":\"printf out; printf err >&2; exit 7\",\"workdir\":\"\",\"timeout_seconds\":10}", 0);
    assert(!strcmp(s, "outerr")); free(s);
    raw = chat_tool_run(&c, "run_command", "{\"command\":\"sleep 20 & wait\",\"workdir\":\"\",\"timeout_seconds\":1}", -1);
    assert(cj_parse(&j, raw, strlen(raw)) && truth(&j, "timed_out")); cj_free(&j); free(raw);
    s = run(&c, "run_command", "{\"command\":\"pwd\",\"workdir\":\"\",\"timeout_seconds\":10}", 1);
    assert(strstr(s, c.workspace)); free(s);
    s = run(&c, "run_command", "{\"command\":\"cat\",\"workdir\":\"\",\"timeout_seconds\":10}", 1); assert(!*s); free(s);
    raw = chat_tool_run(&c, "run_command", "{\"command\":\"yes x | head -c 40000\",\"workdir\":\"\",\"timeout_seconds\":10}", -1);
    assert(cj_parse(&j, raw, strlen(raw)) && truth(&j, "ok") && truth(&j, "truncated"));
    s = cj_string(&j, cj_field(&j, 0, "text")); assert(s && strlen(s) == CHAT_TOOL_TEXT);
    free(s); cj_free(&j); free(raw);
    s = run(&c, "run_command", "{\"command\":\"printf '\\\\377\\\\000OK'\",\"workdir\":\"\",\"timeout_seconds\":10}", 1);
    assert(!strcmp(s, "??OK")); free(s);
    s = run(&c, "read_file", "{}", 0); free(s);
    s = run(&c, "unknown", "{\"path\":\".\"}", 0); free(s);
    s = run(&c, "write_file", "[]", 0); free(s);
    c.workspace[0] = 0;
    s = run(&c, "run_command", "{\"command\":\"echo BAD\",\"workdir\":\"\",\"timeout_seconds\":10}", 0); free(s);
    strcpy(c.workspace, argv[2]);
    assert(chat_atomic(path, "{\"type\":\"function_call\",\"call_id\":\"one\",\"name\":\"run_command\",\"arguments\":\"{\\\"command\\\":\\\"echo once >> receipt\\\",\\\"workdir\\\":\\\"\\\",\\\"timeout_seconds\\\":10}\"}\n"));
    assert(chat_tools_pending(&c, 1, -1) == 1);
    assert(chat_tools_pending(&c, 1, -1) == 0);
    s = run(&c, "read_file", "{\"path\":\"receipt\",\"offset\":0,\"max_bytes\":32768}", 1);
    assert(!strcmp(s, "once\n")); free(s);
    assert(chat_atomic(path, "{\"type\":\"function_call\",\"call_id\":\"two\",\"name\":\"run_command\",\"arguments\":\"{\\\"command\\\":\\\"echo BAD >> receipt\\\",\\\"workdir\\\":\\\"\\\",\\\"timeout_seconds\\\":10}\"}\n"));
    assert(chat_tools_pending(&c, -1, -1) == 1);
    assert(chat_tools_pending(&c, 1, -1) == 0);
    s = run(&c, "read_file", "{\"path\":\"receipt\",\"offset\":0,\"max_bytes\":32768}", 1);
    assert(!strcmp(s, "once\n")); free(s);
    raw = chat_read(path, CHAT_HISTORY); assert(raw && strstr(raw, "interrupted")); free(raw);
    assert(chat_atomic(path, "{\"type\":\"function_call\",\"call_id\":\"duplicate\",\"name\":\"write_file\",\"arguments\":\"{}\"}\n{\"type\":\"function_call\",\"call_id\":\"duplicate\",\"name\":\"write_file\",\"arguments\":\"{}\"}\n"));
    assert(chat_tools_pending(&c, 1, -1) == -1);
    f = chat_temporary(&c); assert(f); chat_tool_definitions(f);
    raw = chat_file_text(f, CHAT_ITEM); fclose(f);
    assert(raw && cj_parse(&j, raw, strlen(raw)) && j.tokens[0].kind == '[');
    cj_free(&j); free(raw);
    f = chat_temporary(&c); assert(f);
    cj_print(f, "safe\033[41m\007\xc2\x9b Ж🙂\n");
    raw = chat_file_text(f, 1024); fclose(f);
    assert(raw && !strcmp(raw, "safe?[41m?? Ж🙂\n")); free(raw);
    {
        char *large = malloc(4096), *arguments = malloc(4300), *output = malloc(4300), *saved;
        assert(large && arguments && output);
        memset(large, 'x', 4000); strcpy(large + 191, "Ж");
        memset(large + 193, 'x', 3807); strcpy(large + 4000, "HIDDEN_TOOL_TAIL");
        sprintf(arguments, "{\"path\":\"long.txt\",\"content\":\"%s\"}", large);
        sprintf(output, "{\"ok\":true,\"text\":\"%s\"}", large);
        f = chat_temporary(&c); assert(f);
        fputs("{\"type\":\"function_call\",\"call_id\":\"compact\",\"name\":\"write_file\",\"arguments\":", f);
        assert(cj_quote(f, arguments));
        fputs("}\n{\"type\":\"function_call_output\",\"call_id\":\"compact\",\"output\":", f);
        assert(cj_quote(f, output)); fputs("}\n", f);
        saved = chat_file_text(f, CHAT_ITEM); fclose(f); assert(saved);
        chat_history_path(&c, path); assert(chat_atomic(path, saved));
        assert(chat_load(&c) && strlen(c.display) < 600 && cj_utf8(c.display, strlen(c.display)));
        assert(strstr(c.display, "long.txt") && strstr(c.display, "F4 expands tools") && !strstr(c.display, "HIDDEN_TOOL_TAIL"));
        strcpy(c.reply, "STREAM_IN_PROGRESS"); c.reply_size = strlen(c.reply);
        assert(chat_tool_view(&c) && strstr(c.display, "HIDDEN_TOOL_TAIL"));
        assert(!strcmp(c.reply, "STREAM_IN_PROGRESS") && c.reply_size == strlen(c.reply));
        assert(chat_tool_view(&c) && !strstr(c.display, "HIDDEN_TOOL_TAIL"));
        assert(!strcmp(c.reply, "STREAM_IN_PROGRESS"));
        raw = chat_read(path, CHAT_HISTORY); assert(raw && !strcmp(raw, saved)); free(raw);
        f = chat_request(&c); assert(f); raw = chat_file_text(f, CHAT_ITEM); fclose(f);
        assert(raw && strstr(raw, "HIDDEN_TOOL_TAIL")); free(raw);
        free(large); free(arguments); free(output); free(saved);
    }
    strcpy(c.workspace, argv[2]);
    pid = fork(); assert(pid >= 0);
    if (!pid) {
        chat_tools_init();
        free(chat_tool_run(&c, "run_command", "{\"command\":\"sleep 20 & wait\",\"workdir\":\"\",\"timeout_seconds\":20}", -1));
        _exit(chat_tools_cancelled() ? 0 : 1);
    }
    usleep(200000); kill(pid, SIGTERM);
    assert(waitpid(pid, &status, 0) == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0);
    free(c.display); free(c.reply);
    puts("Guest tool core passed: Unicode files, exact edits, modes, symlinks, commands, stdin EOF, timeout, cancellation and durable recovery");
    return 0;
}
