#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <ctype.h>
#include "fat16.h"
#include "util.h"
#include "kernel_api.h"

#define PAGE_HEIGHT 22
#define MAX_ARGS 8
#define ARG_LEN 32
#define PATH_PARTS 8

static Fat16Fs g_fs;
static uint16_t g_cwd;
static char g_parts[PATH_PARTS][13];
static int g_nparts;
static int g_page;
static int g_lines;
static int g_abort;

static void print_cwd(void) {
    int i;

    printf("C:");
    if (g_nparts == 0) {
        printf("\\");
    } else {
        i = 0;
        while (i < g_nparts) {
            printf("\\%s", g_parts[i]);
            i = i + 1;
        }
    }
}

static int more_pause(void) {
    int c;

    printf("-- More --");
    c = getchar();
    printf("\r          \r");
    if (c == 'q' || c == 'Q' || c == EOF) {
        return 1;
    }
    return 0;
}

static int more_nl(void) {
    putchar('\n');
    if (!g_page) {
        return 0;
    }
    g_lines = g_lines + 1;
    if (g_lines >= PAGE_HEIGHT) {
        g_lines = 0;
        if (more_pause()) {
            g_abort = 1;
            return 1;
        }
    }
    return 0;
}

static void page_begin(int on) {
    g_page = on;
    g_lines = 0;
    g_abort = 0;
}

static int split_args(char *line, char args[MAX_ARGS][ARG_LEN]) {
    int n;
    int i;
    int a;
    int c;

    n = 0;
    i = 0;
    while (line[i] && n < MAX_ARGS) {
        while (line[i] == ' ' || line[i] == '\t') {
            i = i + 1;
        }
        if (!line[i]) {
            break;
        }
        a = 0;
        if (line[i] == '|') {
            args[n][0] = '|';
            args[n][1] = 0;
            n = n + 1;
            i = i + 1;
            continue;
        }
        while (line[i] && line[i] != ' ' && line[i] != '\t' && line[i] != '|' && a < ARG_LEN - 1) {
            c = line[i];
            args[n][a] = (char)c;
            a = a + 1;
            i = i + 1;
        }
        args[n][a] = 0;
        n = n + 1;
    }
    return n;
}

static int is_p_flag(char *s) {
    if (s[0] == '/' || s[0] == '-') {
        if ((s[1] == 'p' || s[1] == 'P') && s[2] == 0) {
            return 1;
        }
    }
    return 0;
}

static void help(void) {
    puts("DIR [path] [/P]     List directory");
    puts("CD [path]           Change or show directory");
    puts("TYPE file [| MORE]  Show a text file");
    puts("MORE file           Show a text file, paged");
    puts("CLS                 Clear the screen");
    puts("HELP                This list");
    puts("EXIT                Leave the shell");
}

static void cmd_cls(void) {
    int i;

    i = 0;
    while (i < 25) {
        putchar('\n');
        i = i + 1;
    }
}

static int walk_cd(char *path) {
    uint16_t clus;
    int nparts;
    char parts[PATH_PARTS][13];
    int i;
    int n;
    char part[13];
    Fat16DirEnt ent;
    int r;
    int k;

    clus = g_cwd;
    nparts = g_nparts;
    k = 0;
    while (k < g_nparts) {
        i = 0;
        while (g_parts[k][i]) {
            parts[k][i] = g_parts[k][i];
            i = i + 1;
        }
        parts[k][i] = 0;
        k = k + 1;
    }

    i = 0;
    if (path[0] && is_path_sep((int)(unsigned char)path[0])) {
        clus = 0;
        nparts = 0;
        i = 1;
    }

    while (path[i]) {
        while (path[i] && is_path_sep((int)(unsigned char)path[i])) {
            i = i + 1;
        }
        if (!path[i]) {
            break;
        }
        n = 0;
        while (path[i] && !is_path_sep((int)(unsigned char)path[i]) && n < 12) {
            part[n] = path[i];
            n = n + 1;
            i = i + 1;
        }
        part[n] = 0;

        if (n == 1 && part[0] == '.') {
            continue;
        }
        if (n == 2 && part[0] == '.' && part[1] == '.') {
            if (clus == 0 || nparts == 0) {
                clus = 0;
                nparts = 0;
                continue;
            }
            r = fat16_lookup(&g_fs, clus, "..", &ent);
            if (r != FAT16_OK) {
                return r;
            }
            clus = ent.cluster;
            nparts = nparts - 1;
            continue;
        }

        r = fat16_lookup(&g_fs, clus, part, &ent);
        if (r != FAT16_OK) {
            return FAT16_ERR_NOTFOUND;
        }
        if ((ent.attr & FAT16_ATTR_DIR) == 0) {
            return FAT16_ERR_NOTDIR;
        }
        clus = ent.cluster;
        if (nparts >= PATH_PARTS) {
            return FAT16_ERR;
        }
        k = 0;
        while (ent.name[k] && k < 12) {
            parts[nparts][k] = ent.name[k];
            k = k + 1;
        }
        parts[nparts][k] = 0;
        nparts = nparts + 1;
    }

    g_cwd = clus;
    g_nparts = nparts;
    k = 0;
    while (k < nparts) {
        i = 0;
        while (parts[k][i]) {
            g_parts[k][i] = parts[k][i];
            i = i + 1;
        }
        g_parts[k][i] = 0;
        k = k + 1;
    }
    return FAT16_OK;
}

static void print_dirent(Fat16DirEnt *ent) {
    char name8[9];
    char ext3[4];
    int i;
    int n;

    if (ent->name[0] == '.' && ent->name[1] == 0) {
        printf(".            <DIR>");
        return;
    }
    if (ent->name[0] == '.' && ent->name[1] == '.' && ent->name[2] == 0) {
        printf("..           <DIR>");
        return;
    }

    i = 0;
    n = 0;
    while (ent->name[i] && ent->name[i] != '.' && n < 8) {
        name8[n] = ent->name[i];
        n = n + 1;
        i = i + 1;
    }
    while (n < 8) {
        name8[n] = ' ';
        n = n + 1;
    }
    name8[8] = 0;

    ext3[0] = ' ';
    ext3[1] = ' ';
    ext3[2] = ' ';
    ext3[3] = 0;
    if (ent->name[i] == '.') {
        i = i + 1;
        n = 0;
        while (ent->name[i] && n < 3) {
            ext3[n] = ent->name[i];
            n = n + 1;
            i = i + 1;
        }
    }

    if (ent->attr & FAT16_ATTR_DIR) {
        printf("%s %s <DIR>", name8, ext3);
    } else {
        printf("%s %s  %lu", name8, ext3, (unsigned long)ent->size);
    }
}

static void cmd_dir(char *path, int page) {
    uint16_t clus;
    Fat16Dir dir;
    Fat16DirEnt ent;
    int files;
    int dirs;
    int r;

    page_begin(page);
    clus = g_cwd;
    if (path && path[0]) {
        r = fat16_resolve(&g_fs, g_cwd, path, &ent);
        if (r != FAT16_OK || (ent.attr & FAT16_ATTR_DIR) == 0) {
            puts("Invalid directory");
            return;
        }
        clus = ent.cluster;
    }

    printf(" Volume in drive C is %s", g_fs.label);
    more_nl();
    if (g_abort) {
        return;
    }
    printf(" Directory of ");
    print_cwd();
    if (path && path[0] && !is_path_sep((int)(unsigned char)path[0])) {
        printf("\\%s", path);
    }
    more_nl();
    if (g_abort) {
        return;
    }
    more_nl();
    if (g_abort) {
        return;
    }

    files = 0;
    dirs = 0;
    fat16_dir_open(&g_fs, clus, &dir);
    while (fat16_dir_next(&g_fs, &dir, &ent)) {
        print_dirent(&ent);
        if (more_nl()) {
            return;
        }
        if (ent.attr & FAT16_ATTR_DIR) {
            dirs = dirs + 1;
        } else {
            files = files + 1;
        }
    }
    printf("        %d file(s)", files);
    more_nl();
    printf("        %d dir(s)", dirs);
    more_nl();
}

static void cmd_type(char *path, int page) {
    Fat16DirEnt ent;
    int r;
    uint32_t off;
    int n;
    int i;
    uint8_t buf[64];
    int c;

    if (!path || !path[0]) {
        puts("Required parameter missing");
        return;
    }
    r = fat16_resolve(&g_fs, g_cwd, path, &ent);
    if (r != FAT16_OK) {
        puts("File not found");
        return;
    }
    if (ent.attr & FAT16_ATTR_DIR) {
        puts("Access denied");
        return;
    }

    page_begin(page);
    off = 0;
    while (off < ent.size) {
        n = fat16_read_at(&g_fs, ent.cluster, ent.size, off, buf, 64);
        if (n <= 0) {
            break;
        }
        i = 0;
        while (i < n) {
            c = buf[i];
            if (c == '\n') {
                if (more_nl()) {
                    return;
                }
            } else if (c == '\r') {
                /* skip CR in CRLF pairs */
            } else {
                putchar(c);
            }
            i = i + 1;
        }
        off = off + (uint32_t)n;
    }
    if (!g_abort) {
        putchar('\n');
    }
}

static void run_line(char *line) {
    char args[MAX_ARGS][ARG_LEN];
    int n;
    int i;
    int page;
    int more;
    int r;
    char *cmd;
    char *arg = 0;

    n = split_args(line, args);
    if (n == 0) {
        return;
    }

    cmd = args[0];
    if (str_eq_i(cmd, "exit") || str_eq_i(cmd, "quit") || str_eq_i(cmd, "q")) {
        puts("Goodbye.");
        k_exit(0);
        exit(0);
    }
    if (str_eq_i(cmd, "help") || str_eq_i(cmd, "?")) {
        help();
        return;
    }
    if (str_eq_i(cmd, "cls")) {
        cmd_cls();
        return;
    }

    page = 0;
    more = 0;
    i = 1;
    while (i < n) {
        if (is_p_flag(args[i])) {
            page = 1;
        } else if (args[i][0] == '|' && args[i][1] == 0) {
            if (i + 1 < n && str_eq_i(args[i + 1], "more")) {
                more = 1;
                i = i + 1;
            }
        } else if (str_eq_i(args[i], "more") && i == n - 1) {
            more = 1;
        } else if (!arg) {
            arg = args[i];
        }
        i = i + 1;
    }

    if (str_eq_i(cmd, "dir")) {
        cmd_dir(arg, page || more);
        return;
    }
    if (str_eq_i(cmd, "cd") || str_eq_i(cmd, "chdir")) {
        if (!arg) {
            print_cwd();
            putchar('\n');
            return;
        }
        i = walk_cd(arg);
        if (i == FAT16_ERR_NOTFOUND) {
            puts("Invalid directory");
        } else if (i == FAT16_ERR_NOTDIR) {
            puts("Invalid directory");
        } else if (i != FAT16_OK) {
            puts("Invalid directory");
        }
        return;
    }
    if (str_eq_i(cmd, "type")) {
        cmd_type(arg, more || page);
        return;
    }
    if (str_eq_i(cmd, "more")) {
        cmd_type(arg, 1);
        return;
    }

    r = k_exec(cmd);
    if (r == FAT16_ERR_NOTFOUND) {
        printf("Bad command or file name\n");
    }
}

int main(void) {
    char line[80];
    int r;

    r = fat16_mount(&g_fs);
    if (r != FAT16_OK) {
        puts("Cannot mount FAT16 disk.");
        return 1;
    }

    g_cwd = 0;
    g_nparts = 0;

    printf("rOS FAT16 explorer  Volume %s\n", g_fs.label);
    puts("Type HELP for commands.");

    while (1) {
        print_cwd();
        printf("> ");
        read_line(line, 80);
        run_line(line);
    }
    return 0;
}
