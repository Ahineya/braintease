/* LGeneral's optional long options, without a guest libc dependency. */
#ifndef LG_CLI_H
#define LG_CLI_H
#include "lg-types.h"
struct option { const char *name; int has_arg; lg_int *flag; int val; };
static char *lg_optarg;
static int lg_optind = 1;
#define optarg lg_optarg
#define optind lg_optind
static int getopt_long(lg_int argc, char **argv, const char *shorts,
                       const struct option *options, void *unused)
{
    int i; char *arg, *eq; size_t len;
    (void)shorts; (void)unused;
    if (lg_optind >= argc || argv[lg_optind][0] != '-') return -1;
    arg = argv[lg_optind++];
    if (!strcmp(arg, "--")) return -1;
    if (strncmp(arg, "--", 2)) { fprintf(stderr, "Unknown option: %s\n", arg); return '?'; }
    arg += 2; eq = strchr(arg, '='); len = eq ? (size_t)(eq-arg) : strlen(arg);
    for (i=0; options[i].name; ++i) {
        if (strlen(options[i].name)!=len || strncmp(arg, options[i].name,len)) continue;
        lg_optarg = eq ? eq+1 : 0;
        if (options[i].has_arg && !lg_optarg) {
            if (lg_optind>=argc) { fprintf(stderr,"Option needs a value: --%s\n",arg); return '?'; }
            lg_optarg=argv[lg_optind++];
        }
        if (!options[i].has_arg && eq) return '?';
        if (options[i].flag) { *options[i].flag=options[i].val; return 0; }
        return options[i].val;
    }
    fprintf(stderr,"Unknown option: --%s\n",arg); return '?';
}
#endif
