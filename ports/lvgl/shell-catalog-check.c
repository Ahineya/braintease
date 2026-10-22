/* SPDX-License-Identifier: GPL-2.0-or-later */
#define _POSIX_C_SOURCE 200809L
#include "shell-catalog.h"
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static unsigned checks;
static char last_child_app[64];
#define CHECK(e) do { checks++; if (!(e)) { fprintf(stderr, "%s:%d: %s (errno %d)\n", __FILE__, __LINE__, #e, errno); exit(1); } } while (0)
static void write_file(const char *path, const char *text) {
    FILE *f = fopen(path, "w"); CHECK(f != NULL); CHECK(fputs(text, f) >= 0); CHECK(!fclose(f));
}
static void path(char *out, const char *root, const char *name) { snprintf(out, 1024, "%s/%s", root, name); }
static void entry(char *out, const char *id, const char *name, const char *category, const char *program, const char *extra) {
    snprintf(out, 8192, "[application]\nid=%s\nname=%s\nicon=terminal\ncategory=%s\nkeywords=hello WORLD\nkind=gui\nexec=%s\npinned=true\n%s", id, name, category, program, extra);
}
static void pause_poll(void) { struct timespec t = {0, 1000000}; nanosleep(&t, NULL); }
static void child_events(ShellCatalog *c, pid_t child, int failure, int exit_code) {
    ShellLaunchEvent event;
    unsigned i, phase = 0;
    for (i = 0; i < 5000 && phase != 2; i++) {
        if (!shell_catalog_poll(c, &event)) { pause_poll(); continue; }
        CHECK(event.pid == child);
        if (!phase) {
            CHECK(event.state == (failure ? SHELL_LAUNCH_FAILED : SHELL_LAUNCH_STARTED));
            CHECK(failure ? event.error != 0 : event.error == 0);
            snprintf(last_child_app, sizeof(last_child_app), "%s", event.app_id);
        } else {
            CHECK(event.state == SHELL_LAUNCH_EXITED);
            CHECK(WIFEXITED(event.status)); CHECK(WEXITSTATUS(event.status) == exit_code);
        }
        phase++;
    }
    CHECK(phase == 2); CHECK(!shell_catalog_poll(c, &event));
    errno = 0; CHECK(waitpid(child, NULL, WNOHANG) == -1 && errno == ECHILD);
}
static void capture_check(const char *file, const char **arguments, size_t count) {
    FILE *f = fopen(file, "r"); size_t i;
    CHECK(f != NULL);
    for (i = 0; i < count; i++) {
        char buffer[4096]; size_t used = 0; int ch;
        while ((ch = fgetc(f)) != EOF && ch) { CHECK(used + 1 < sizeof(buffer)); buffer[used++] = (char)ch; }
        buffer[used] = 0; CHECK(ch == 0);
        if (strcmp(buffer, arguments[i])) fprintf(stderr, "Argument %lu: got <%s>, expected <%s>\n", (unsigned long)i, buffer, arguments[i]);
        CHECK(!strcmp(buffer, arguments[i]));
    }
    CHECK(fgetc(f) == EOF); fclose(f);
}
int main(int argc, char **argv) {
    const char *capture = getenv("SHELL_TEST_CAPTURE");
    char config[1024], pins[1024], output[1024], executable[1024], text[8192], directory[1024], name[80];
    const char *root;
    ShellCatalog *c, *other;
    const ShellApp *app;
    pid_t child, unrelated;
    size_t i;
    if (capture) {
        FILE *f = fopen(capture, "w"); if (!f) return 90;
        for (i = 0; i < (size_t)argc; i++) fwrite(argv[i], 1, strlen(argv[i]) + 1, f);
        fclose(f); return 23;
    }
    CHECK(argc == 4); root = argv[1];
    path(config, root, "app.ini"); path(pins, root, "pins.ini"); path(output, root, "capture");
    path(executable, root, "executable with spaces"); path(directory, root, "many");
    CHECK(!symlink(argv[2], executable));
    c = shell_catalog_new(); CHECK(c != NULL);
    CHECK(!shell_catalog_load(c, argv[3])); CHECK(shell_catalog_count(c) == 13);
    CHECK(shell_catalog_find(c, "org.riplix.Calculator") != NULL);
    CHECK(shell_catalog_find(c, "org.riplix.Commander")->kind == SHELL_APP_TERMINAL);
    other = shell_catalog_new(); CHECK(other != NULL);
    CHECK(!mkdir(directory, 0700));
    for (i = 0; i < 300; i++) {
        char filename[1024]; snprintf(name, sizeof(name), "org.test.App%03lu", (unsigned long)i);
        snprintf(filename, sizeof(filename), "%s/%03lu.ini", directory, (unsigned long)i);
        entry(text, name, name, i % 2 ? "Utilities" : "Development", executable, ""); write_file(filename, text);
    }
    CHECK(!shell_catalog_load(other, directory)); CHECK(shell_catalog_count(other) == 300);
    for (i = 1; i < shell_catalog_count(other); i++) CHECK(strcmp(shell_catalog_at(other, i - 1)->category, shell_catalog_at(other, i)->category) <= 0);
    CHECK(shell_catalog_load(other, directory) < 0); CHECK(shell_catalog_count(other) == 300);
    shell_catalog_free(other);

    entry(text, "org.test.Capture", "A Capture", "Development", executable,
          "arg1=two words\narg2=\"double quote\"\narg3=$HOME; `false`; $(false)\narg4=\n");
    write_file(config, text); CHECK(!shell_catalog_load(c, config));
    app = shell_catalog_find(c, "org.test.Capture"); CHECK(app != NULL); CHECK(app->argument_count == 4);
    CHECK(shell_app_matches(app, "cap HELLO development")); CHECK(shell_app_matches(app, ""));
    CHECK(!shell_app_matches(app, "calculator")); CHECK(shell_app_matches(app, "world"));
    CHECK(shell_app_available(c, app) != NULL);
    CHECK(shell_catalog_load(c, config) < 0); CHECK(shell_catalog_count(c) == 14);
    CHECK(!shell_pins_load(c, pins)); CHECK(shell_app_pinned(c, app));
    write_file(pins, "[pins]\norg.test.Capture=false\norg.future.Unavailable=true\n");
    CHECK(!shell_pins_load(c, pins)); CHECK(!shell_app_pinned(c, app));
    CHECK(shell_pin_set(c, "invalid id", 1) < 0); CHECK(shell_pin_set(c, "org.future.Unavailable", 2) < 0);
    CHECK(!shell_pin_set(c, app->id, 1)); CHECK(!shell_pins_save(c, pins));
    CHECK(!shell_pins_load(c, pins)); CHECK(shell_app_pinned(c, app));
    {
        FILE *f = fopen(pins, "r"); char contents[1024]; size_t got;
        CHECK(f != NULL); got = fread(contents, 1, sizeof(contents) - 1, f); contents[got] = 0; fclose(f);
        CHECK(strstr(contents, "org.future.Unavailable=true") != NULL);
    }
    {
        char temporary[1100];
        snprintf(temporary, sizeof(temporary), "%s.tmp.%ld", pins, (long)getpid());
        write_file(temporary, "occupied temporary file\n");
        CHECK(!shell_pin_set(c, app->id, 0)); CHECK(shell_pins_save(c, pins) < 0);
        CHECK(!unlink(temporary)); CHECK(!shell_pins_load(c, pins)); CHECK(shell_app_pinned(c, app));
    }
    write_file(pins, "[pins]\norg.test.Capture=false\norg.test.Capture=true\n");
    CHECK(shell_pins_load(c, pins) < 0); CHECK(shell_app_pinned(c, app));
    CHECK(!unlink(pins)); CHECK(!shell_pins_load(c, pins)); CHECK(shell_app_pinned(c, app));

    CHECK(!setenv("SHELL_TEST_CAPTURE", output, 1));
    unrelated = fork(); CHECK(unrelated >= 0); if (!unrelated) _exit(17);
    child = shell_catalog_launch(c, app); CHECK(child > 0); child_events(c, child, 0, 23);
    {
        const char *expected[] = {executable, "two words", "double quote", "$HOME; `false`; $(false)", ""};
        capture_check(output, expected, 5);
    }
    { int status; CHECK(waitpid(unrelated, &status, 0) == unrelated); CHECK(WEXITSTATUS(status) == 17); }
    CHECK(!unsetenv("SHELL_TEST_CAPTURE"));

    /* Dynamic availability observes chmod changes and the explicit fallback. */
    path(executable, root, "not executable"); write_file(executable, "not a program\n");
    entry(text, "org.test.Fallback", "Fallback", "Development", executable, "fallback=/bin/echo\n");
    write_file(config, text); CHECK(!shell_catalog_load(c, config));
    app = shell_catalog_find(c, "org.test.Fallback"); CHECK(app != NULL);
    CHECK(!strcmp(shell_app_available(c, app), "/bin/echo"));
    CHECK(!chmod(executable, 0700)); CHECK(!strcmp(shell_app_available(c, app), executable));
    child = shell_catalog_launch(c, app); CHECK(child > 0); child_events(c, child, 1, 127);
    CHECK(!unlink(executable)); CHECK(!strcmp(shell_app_available(c, app), "/bin/echo"));

    /* Terminal argv includes the app identity and an explicit -- boundary. */
    snprintf(text, sizeof(text), "[application]\nid=org.test.Tui\nname=Adult TUI\nicon=folder\ncategory=System\nkind=terminal\nexec=/bin/echo\ncolumns=90\nrows=29\narg1=--literal program argument\n");
    write_file(config, text); CHECK(!shell_catalog_load(c, config));
    CHECK(!shell_catalog_terminal(c, argv[2], ""));
    app = shell_catalog_find(c, "org.test.Tui"); CHECK(app != NULL);
    CHECK(!setenv("SHELL_TEST_CAPTURE", output, 1));
    child = shell_catalog_launch(c, app); CHECK(child > 0); child_events(c, child, 0, 23);
    {
        const char *expected[] = {argv[2], "-g", "90x29", "--app-id", "org.test.Tui", "--app-name", "Adult TUI",
                                  "--app-icon", "folder", "--", "/bin/echo", "--literal program argument"};
        capture_check(output, expected, 12);
    }
    CHECK(!unsetenv("SHELL_TEST_CAPTURE"));
    CHECK(!shell_catalog_terminal(c, "/no/such/gterm", "")); CHECK(!shell_app_available(c, app));
    CHECK(shell_catalog_launch(c, app) < 0 && errno == ENOENT);

    {
        const char *bad[] = {
            "id=another\n", "arg0=x\n", "arg2=hole\n", "arg1=x\narg1=y\n", "arg999999999999999999999999=x\n",
            "kind=terminal\n", "unknown=value\n", "columns=80\n", "name=again\n", "arg1=bad\001control\n"
        };
        size_t before = shell_catalog_count(c);
        for (i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
            entry(text, "org.test.Invalid", "Invalid", "Development", "/bin/echo", bad[i]);
            write_file(config, text); CHECK(shell_catalog_load(c, config) < 0); CHECK(shell_catalog_count(c) == before);
        }
        entry(text, "invalid id", "Invalid", "Development", "/bin/echo", "");
        write_file(config, text); CHECK(shell_catalog_load(c, config) < 0);
        entry(text, "org.test.Invalid", "Invalid", "Development", "echo", "");
        write_file(config, text); CHECK(shell_catalog_load(c, config) < 0);
        entry(text, "org.test.Invalid", "Invalid\xc0\xaf", "Development", "/bin/echo", "");
        write_file(config, text); CHECK(shell_catalog_load(c, config) < 0);
        memset(name, 'n', 64); name[64] = 0;
        entry(text, "org.test.Invalid", name, "Development", "/bin/echo", "");
        write_file(config, text); CHECK(shell_catalog_load(c, config) < 0);
        entry(text, "org.test.Unicode", "\xd0\xa0\xd1\x96\xd0\xbf\xd0\xbb\xd1\x96\xd0\xba\xd1\x81 \xf0\x9f\x8c\x8d", "Development", "/bin/echo", "");
        write_file(config, text); CHECK(!shell_catalog_load(c, config));
        CHECK(shell_catalog_find(c, "org.test.Unicode") != NULL);
    }
    {
        const char *known = shell_catalog_find(c, "org.test.Capture")->executable;
        const char *expected[] = {known, "two words", "double quote", "$HOME; `false`; $(false)", "", "extra value"};
        const char *commands[] = {"org.test.Capture 'extra value'", "\"a capture\" 'extra value'", "\"executable with spaces\" 'extra value'"};
        CHECK(!setenv("SHELL_TEST_CAPTURE", output, 1));
        for (i = 0; i < sizeof(commands) / sizeof(commands[0]); i++) {
            child = shell_catalog_run(c, commands[i], SHELL_RUN_AUTO);
            CHECK(child > 0); child_events(c, child, 0, 23);
            CHECK(!strcmp(last_child_app, "org.test.Capture")); capture_check(output, expected, 6);
        }
        snprintf(text, sizeof(text), "\"%s\" 'extra value'", known);
        child = shell_catalog_run(c, text, SHELL_RUN_AUTO);
        CHECK(child > 0); child_events(c, child, 0, 23);
        CHECK(!strcmp(last_child_app, "org.test.Capture")); capture_check(output, expected, 6);
    }
    {
        const char *expected[] = {argv[2], "two words", "quoted \"text\"", "path with spaces", "", "$HOME; `x`; $(x)", "back\\slash"};
        snprintf(text, sizeof(text), "\"%s\" 'two words' \"quoted \\\"text\\\"\" path\\ with\\ spaces '' '$HOME; `x`; $(x)' 'back\\slash'", argv[2]);
        child = shell_catalog_run(c, text, SHELL_RUN_AUTO);
        CHECK(child > 0); child_events(c, child, 0, 23); capture_check(output, expected, 7);
        CHECK(!strncmp(last_child_app, "org.riplix.Run.", 15));
    }
    {
        char bin[1024], runner[1100], first_id[64], cwd[1024], run_cwd[1024];
        const char *parent_path = getenv("PATH");
        char *saved_path = parent_path ? strdup(parent_path) : NULL;
        path(bin, root, "run-bin"); CHECK(!mkdir(bin, 0700));
        snprintf(runner, sizeof(runner), "%s/runner", bin); CHECK(!symlink(argv[2], runner));
        CHECK(!setenv("PATH", bin, 1));
        child = shell_catalog_run(c, "runner hello", SHELL_RUN_AUTO);
        CHECK(child > 0); child_events(c, child, 0, 23);
        {
            const char *expected[] = {runner, "hello"}; capture_check(output, expected, 2);
        }
        snprintf(first_id, sizeof(first_id), "%s", last_child_app);
        CHECK(!shell_catalog_terminal(c, argv[2], ""));
        child = shell_catalog_run(c, "runner 'hello world'", SHELL_RUN_TERMINAL);
        CHECK(child > 0); child_events(c, child, 0, 23); CHECK(!strcmp(first_id, last_child_app));
        {
            const char *expected[] = {argv[2], "-g", "80x28", "--app-id", first_id, "--app-name", "runner",
                                     "--app-icon", "terminal", "--", runner, "hello world"};
            capture_check(output, expected, 12);
        }
        child = shell_catalog_run(c, "org.test.Tui extra", SHELL_RUN_AUTO);
        CHECK(child > 0); child_events(c, child, 0, 23); CHECK(!strcmp(last_child_app, "org.test.Tui"));
        {
            const char *expected[] = {argv[2], "-g", "90x29", "--app-id", "org.test.Tui", "--app-name", "Adult TUI",
                                     "--app-icon", "folder", "--", "/bin/echo", "--literal program argument", "extra"};
            capture_check(output, expected, 13);
        }
        CHECK(getcwd(cwd, sizeof(cwd)) != NULL); CHECK(!chdir(bin)); CHECK(getcwd(run_cwd, sizeof(run_cwd)) != NULL);
        child = shell_catalog_run(c, "./runner relative", SHELL_RUN_AUTO);
        CHECK(child > 0); child_events(c, child, 0, 23);
        {
            char relative[1100]; const char *expected[] = {relative, "relative"};
            snprintf(relative, sizeof(relative), "%s/./runner", run_cwd); capture_check(output, expected, 2);
        }
        CHECK(!chdir(cwd));
        CHECK(shell_catalog_run(c, "missing-program", SHELL_RUN_AUTO) < 0);
        CHECK(!unsetenv("SHELL_TEST_CAPTURE")); CHECK(!unsetenv("PATH"));
        child = shell_catalog_run(c, "true", SHELL_RUN_AUTO); CHECK(child > 0); child_events(c, child, 0, 0);
        if (saved_path) { CHECK(!setenv("PATH", saved_path, 1)); free(saved_path); }
        else CHECK(!unsetenv("PATH"));
    }
    {
        const char *bad[] = {"", " \t ", "''", "runner 'unfinished", "runner \\", "runner\nnext", "runner \"unfinished"};
        for (i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) CHECK(shell_catalog_run(c, bad[i], SHELL_RUN_AUTO) < 0);
        memset(text, 'a', 4097); text[4097] = 0;
        CHECK(shell_catalog_run(c, text, SHELL_RUN_AUTO) < 0);
        CHECK(shell_catalog_run(c, "true", (enum ShellRunMode)99) < 0);
        path(executable, root, "run-no-exec"); write_file(executable, "no execute flag\n");
        snprintf(text, sizeof(text), "\"%s\"", executable);
        CHECK(shell_catalog_run(c, text, SHELL_RUN_AUTO) < 0);
        CHECK(!chmod(executable, 0700));
        child = shell_catalog_run(c, text, SHELL_RUN_AUTO); CHECK(child > 0); child_events(c, child, 1, 127);
    }
    shell_catalog_free(c);
    printf("Graphite catalog: %u checks passed\n", checks);
    return 0;
}
