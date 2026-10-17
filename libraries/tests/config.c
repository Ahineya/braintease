/* Shared C APIs: run on a normal host as well as through the RFL OS tests. */
#include "../ini/ini.h"
#include "../tui-theme/tui_theme.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
static void write_file(const char *dir, const char *name, const char *text) {
    char path[512]; FILE *file;
    snprintf(path, sizeof(path), "%s/%s.ini", dir, name);
    file = fopen(path, "w"); assert(file);
    assert(fputs(text, file) >= 0); assert(!fclose(file));
}
static void parser(void) {
    const char *text = "; comment\r\n[section]\r\n key = 'hello = # world'\r\nkey = final\n";
    IniReader *r = ini_reader(text, strlen(text));
    char *large = malloc(65537); unsigned i;
    assert(r && large);
    assert(ini_next(r) == 1 && ini_line(r) == 3);
    assert(!strcmp(ini_section(r), "section") && !strcmp(ini_key(r), "key"));
    assert(!strcmp(ini_value(r), "hello = # world"));
    assert(ini_next(r) == 1 && !strcmp(ini_value(r), "final"));
    assert(ini_next(r) == 0 && ini_next(r) == 0); ini_free(r);
    r = ini_reader("[x]\nbroken", 10); assert(ini_next(r) == INI_SYNTAX && ini_line(r) == 2); ini_free(r);
    r = ini_reader("x=a\0b", 5); assert(ini_next(r) == INI_SYNTAX); ini_free(r);
    memset(large, 'a', 1025); r = ini_reader(large, 1025); assert(ini_next(r) == INI_LIMIT); ini_free(r);
    r = ini_reader(NULL, 65537); assert(ini_next(r) == INI_LIMIT); ini_free(r);
    large[0] = 'x'; large[1] = '='; memset(large + 2, 'a', 1022);
    r = ini_reader(large, 1024); assert(ini_next(r) == 1 && strlen(ini_value(r)) == 1022); ini_free(r);
    for (i = 0; i < 257; ++i) memcpy(large + i * 4, "x=y\n", 4);
    r = ini_reader(large, 257 * 4);
    for (i = 0; i < 256; ++i) assert(ini_next(r) == 1);
    assert(ini_next(r) == INI_LIMIT); ini_free(r); free(large);
}
static void themes(const char *dir) {
    TuiTheme *t = tui_theme_new("Application defaults", 1);
    char path[512], name[25], text[128]; unsigned i;
    assert(t && tui_theme_define(t, "custom", 7, 4, 0) == 0);
    assert(tui_theme_define(t, "selected", 0, 6, TUI_BOLD) == 0);
    assert(tui_theme_count(t) == 2 && tui_theme_define(t, "custom", 0, 0, 0) < 0);
    assert(!tui_theme_valid_name("../bad") && tui_theme_valid_name("Good-theme_2"));
    write_file(dir, "base", "[theme]\nname=Base\n[colors]\ncustom=yellow, black, bold\n");
    write_file(dir, "child", "[theme]\nbase=base\nname=Child\nborders=ascii\n[colors]\nselected=default, 255, reverse, underline\n");
    assert(!tui_theme_named(t, dir, "child"));
    assert(!strcmp(tui_theme_name(t), "Child") && !tui_theme_lines(t));
    assert(tui_theme_fg(t, 0) == 3 && tui_theme_bg(t, 0) == 0 && tui_theme_flags(t, 0) == TUI_BOLD);
    assert(tui_theme_fg(t, 1) == -1 && tui_theme_bg(t, 1) == 255 && tui_theme_flags(t, 1) == 6);
    write_file(dir, "broken", "[theme]\nname=Half applied\n[colors]\ncustom=256, blue\n");
    assert(tui_theme_named(t, dir, "broken") < 0 && !strcmp(tui_theme_name(t), "Child"));
    assert(tui_theme_bg(t, 1) == 255 && !tui_theme_lines(t));
    write_file(dir, "cycle-a", "[theme]\nbase=cycle-b\n");
    write_file(dir, "cycle-b", "[theme]\nbase=cycle-a\n");
    assert(tui_theme_named(t, dir, "cycle-a") < 0 && strstr(tui_theme_error(t), "cycle"));
    for (i = 0; i < 9; ++i) {
        snprintf(name, sizeof(name), "depth-%u", i);
        snprintf(text, sizeof(text), "[theme]\nbase=depth-%u\n", i + 1);
        write_file(dir, name, i == 8 ? "[theme]\nname=Deep\n" : text);
    }
    assert(tui_theme_named(t, dir, "depth-0") < 0 && strstr(tui_theme_error(t), "eight"));
    assert(tui_theme_named(t, dir, "depth-1") == 0); /* Exactly eight files. */
    write_file(dir, "layer", "[theme]\nbase=../ignored\nbase=base\n[colors]\nselected=black, white\n");
    snprintf(path, sizeof(path), "%s/layer.ini", dir);
    assert(!tui_theme_file(t, dir, path, 0));
    assert(tui_theme_bg(t, 0) == 0 && tui_theme_bg(t, 1) == 7);
    write_file(dir, "layer", "[colors]\ncustom=white, magenta\n");
    assert(!tui_theme_file(t, dir, path, 0) && tui_theme_bg(t, 1) == 7 && tui_theme_bg(t, 0) == 5);
    assert(unlink(path) == 0 && !tui_theme_file(t, dir, path, 1));
    assert(tui_theme_file(t, dir, path, 0) < 0 && tui_theme_bg(t, 0) == 5);
    assert(tui_theme_apply(t, "unknown", "x", "y") < 0);
    assert(tui_theme_apply(t, "colors", "custom", "black, blue, unknown") < 0);
    assert(tui_theme_bg(t, 0) == 5);
    tui_theme_free(t);
}
int main(int argc, char **argv) {
    assert(argc == 2); parser(); themes(argv[1]);
    puts("Shared INI and theme C APIs passed"); return 0;
}
