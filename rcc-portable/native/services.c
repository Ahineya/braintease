/* Process requests use the existing continuation ABI, never host libc calls. */
#include "native.h"
#include <string.h>

int native_service(struct module *m, const char *name)
{
    if (native_find_function(m, name))
        return 0;
    return !strcmp(name, "rcc_hosted_syscall") ? 12 : 0;
}

int native_process_service(const char *name, unsigned *arity)
{
    static const struct {
        const char *name;
        unsigned op, arity;
    } services[] = {{"rcc_process_fork", 1, 0},
                    {"rcc_process_yield", 2, 0},
                    {"rcc_process_wait", 3, 2},
                    {"rcc_process_exit", 4, 1},
                    {"exit", 4, 1},
                    {"rcc_process_getpid", 5, 0},
                    {"rcc_process_kernel_alloc", 6, 1},
                    {"rcc_process_kernel_free", 7, 1},
                    {"rcc_hosted_clone", 8, 1},
                    {"rcc_hosted_switch", 9, 1},
                    {"rcc_hosted_drop_switch", 10, 1},
                    {"rcc_hosted_exec", 11, 2},
                    {"rcc_hosted_syscall", 12, 1},
                    {"setjmp", 13, 1},
                    {"longjmp", 14, 2}};

    size_t i;
    for (i = 0; i < sizeof services / sizeof *services; i++)
        if (!strcmp(name, services[i].name)) {
            *arity = services[i].arity;
            return (int)services[i].op;
        }
    return 0;
}

int native_kernel_import(struct module *m, const char *name)
{
    static const struct {
        const char *name;
        unsigned arity;
    } services[] = {

#include "services-table.h"
    };
    size_t i;
    if (native_find_function(m, name))
        return 0;
    for (i = 0; i < sizeof services / sizeof *services; i++)
        if (!strcmp(name, services[i].name))
            return (int)services[i].arity + 1;
    return 0;
}
