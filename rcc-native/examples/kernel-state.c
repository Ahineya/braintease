#include <native_process.h>
extern int puts(const char *);

struct KernelState { int completed; int status; };
static int private_global = 7;

int main(void)
{
    struct KernelState *kernel = rcc_process_kernel_alloc(sizeof(struct KernelState));
    if (!kernel)
        return 1;
    kernel->completed = 0;
    int pid = rcc_process_fork();
    if (pid == 0) {
        private_global = 99;
        kernel->completed = 1;
        return 23;
    }
    if (pid < 0 || rcc_process_wait(pid, &kernel->status) != pid)
        return 2;
    if (kernel->completed != 1 || kernel->status != 23 || private_global != 7)
        return 3;
    rcc_process_kernel_free(kernel);
    puts("shared kernel state; private process globals");
    return 0;
}
