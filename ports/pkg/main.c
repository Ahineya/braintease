#include "pkg.h"
#include <stdlib.h>
#include <string.h>
#ifndef PKG_TARGET
#define PKG_TARGET "host"
#endif
int pkg_adopt_seed, pkg_discard_previous;
static void usage(void) {
    puts("pkg [--root DIR] [--target ABI] [--adopt-seed] [--discard-previous] COMMAND\n"
         "  create MANIFEST STAGING OUTPUT.rpk\n"
         "  inspect FILE.rpk | hash FILE\n"
         "  install FILE.rpk | remove NAME | list | info NAME\n"
         "  recover | clean | repo DIRECTORY\n  update | search [TEXT] | add NAME | verify INDEX "
         "SIGNATURE "
         "PUBLIC_KEY\n"
         "Packages contain regular userspace files; no installation scripts.");
}
int pkg_cli(int argc, char **argv) {
    const char *root = getenv("PKG_ROOT"), *target = getenv("PKG_TARGET");
    int at = 1, result = -1;
    Package *p = NULL;
    if (!root)
        root = "/";
    if (!target)
        target = PKG_TARGET;
    pkg_error[0] = 0;
    pkg_adopt_seed = 0;
    pkg_discard_previous = 0;
    while (at < argc && argv[at][0] == '-') {
        if (!strcmp(argv[at], "--help")) {
            usage();
            return 0;
        }
        if (!strcmp(argv[at], "--adopt-seed")) {
            pkg_adopt_seed = 1;
            at++;
            continue;
        }
        if (!strcmp(argv[at], "--discard-previous")) {
            pkg_discard_previous = 1;
            at++;
            continue;
        }
        if (at + 1 >= argc)
            break;
        if (!strcmp(argv[at], "--root"))
            root = argv[++at];
        else if (!strcmp(argv[at], "--target"))
            target = argv[++at];
        else
            break;
        at++;
    }
    if (at >= argc) {
        usage();
        return 2;
    }
    if (!strcmp(argv[at], "create") && argc - at == 4)
        result = pkg_create(argv[at + 1], argv[at + 2], argv[at + 3]);
    else if (!strcmp(argv[at], "hash") && argc - at == 2) {
        char hash[65];
        unsigned long size;
        result = pkg_hash_file(argv[at + 1], hash, &size);
        if (!result)
            printf("%s %lu\n", hash, size);
    } else if (!strcmp(argv[at], "inspect") && argc - at == 2) {
        p = calloc(1, sizeof(*p));
        if (p) {
            result = pkg_inspect(argv[at + 1], p, NULL);
            if (!result)
                printf("%s %s [%s]\nfiles=%u\nsha256=%s\ndepends=%s\n", p->name, p->version,
                       p->target, p->count, p->hash, p->depends);
        }
    } else if (!strcmp(argv[at], "install") && argc - at == 2)
        result = pkg_install(root, target, argv[at + 1]);
    else if (!strcmp(argv[at], "remove") && argc - at == 2)
        result = pkg_remove(root, argv[at + 1]);
    else if (!strcmp(argv[at], "list") && argc - at == 1)
        result = pkg_query(root, NULL);
    else if (!strcmp(argv[at], "info") && argc - at == 2)
        result = pkg_query(root, argv[at + 1]);
    else if (!strcmp(argv[at], "recover") && argc - at == 1)
        result = pkg_recover(root);
    else if (!strcmp(argv[at], "clean") && argc - at == 1)
        result = pkg_clean(root);
    else if (!strcmp(argv[at], "repo") && argc - at == 2)
        result = pkg_repo(argv[at + 1]);
    else if (!strcmp(argv[at], "update") && argc - at == 1)
        result = pkg_update(root);
    else if (!strcmp(argv[at], "search") && argc - at <= 2)
        result = pkg_search(root, target, argc - at == 2 ? argv[at + 1] : "");
    else if (!strcmp(argv[at], "add") && argc - at == 2)
        result = pkg_add(root, target, argv[at + 1]);
    else if (!strcmp(argv[at], "verify") && argc - at == 4)
        result = pkg_verify(argv[at + 1], argv[at + 2], argv[at + 3]);
    else {
        usage();
        return 2;
    }
    free(p);
    if (result) {
        fprintf(stderr, "pkg: %s\n", pkg_error[0] ? pkg_error : "operation failed");
        return 1;
    }
    return 0;
}
#ifndef PKG_NO_MAIN
int main(int argc, char **argv) { return pkg_cli(argc, argv); }
#endif
