/* SPDX-License-Identifier: MIT */
#include "chat.h"
#include "json.h"
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
static volatile sig_atomic_t stopped;
static void stop(int sig) {
    (void)sig;
    stopped = 1;
}
int main(int argc, char **argv) {
    Chat *c = calloc(1, sizeof(*c));
    int ok = 0, at = 1;
    if (!c)
        return 1;
    if (!chat_open(c)) {
        fprintf(stderr, "chat: %s\n", c->status);
        free(c);
        return 1;
    }
    if (argc >= 3 && !strcmp(argv[1], "--workspace")) {
        if (!chat_workspace(c, argv[2])) {
            fprintf(stderr, "chat: %s\n", c->status);
            chat_close(c); free(c); return 1;
        }
        at = 3;
    }
    argv += at - 1;
    argc -= at - 1;
    if (argc == 1)
        ok = chat_ui(c) == 0;
    else if (argc == 2 && !strcmp(argv[1], "--probe")) {
        ok = chat_probe(c);
        puts(c->status);
    } else if (argc == 3 && !strcmp(argv[1], "--login-request")) {
        ok = chat_login_request(c, argv[2]);
        puts(ok ? "Login request written. On your Mac run ports/chat/login.py --request FILE "
                  "--output CREDENTIALS."
                : "Could not write login request");
    } else if (argc == 3 && !strcmp(argv[1], "--import")) {
        ok = chat_import(c, argv[2]);
        puts(c->status);
    } else if (argc == 3 && !strcmp(argv[1], "--instructions")) {
        ok = chat_instructions(c, argv[2]);
        puts(c->status);
    } else if (argc == 2 && !strcmp(argv[1], "--list")) {
        unsigned i;
        for (i = 0; i < c->count; i++)
            printf("%u %s\n", c->ids[i], c->titles[i]);
        ok = 1;
    } else if (argc == 2 && (!strcmp(argv[1], "--history") || !strcmp(argv[1], "--history-full"))) {
        if (!strcmp(argv[1], "--history-full")) { c->expand_tools = 1; chat_load(c); }
        cj_print(stdout, c->display);
        putchar('\n');
        ok = 1;
    } else if (argc == 3 && !strcmp(argv[1], "--ask")) {
        signal(SIGINT, stop);
        signal(SIGTERM, stop);
        c->echo = 1;
        ok = chat_send(c, argv[2]);
        while (ok && c->worker && !stopped) {
            chat_step(c);
            usleep(20000UL);
        }
        if (stopped) {
            chat_cancel(c);
            ok = 0;
        } else
            ok = ok && c->outcome == 1;
        putchar('\n');
        fprintf(stderr, "%s\n", c->status);
    } else {
        puts("Fuse Chat | GPT-6.1 Sol Light\nUsage: chat [--workspace DIR] [--probe | --login-request FILE | "
             "--import FILE | --instructions FILE | --ask TEXT | --list | --history | --history-full]\nWithout options, opens the "
             "terminal chat. History and credentials: $HOME/.chat (or CHAT_HOME).");
    }
    chat_close(c);
    free(c);
    return ok ? 0 : 1;
}
