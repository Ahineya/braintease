#include "link.h"
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>

static char *pending;

static void cleanup(void)
{
    if (pending)
        unlink(pending);
}

int main(int argc, char **argv)
{
    const char *output = NULL, *metadata = NULL, *sdk = NULL;
    const char *efi_loader = NULL, *kernel_payload = NULL;
    const char *boot_media = NULL;
    int i, fd;
    FILE *f;
    mode_t mask;
    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-o") && i + 1 < argc && !output)
            output = argv[++i];
        else if (!strcmp(argv[i], "--metadata") && i + 1 < argc && !metadata)
            metadata = argv[++i];
        else if (!strcmp(argv[i], "--sdk") && i + 1 < argc && !sdk)
            sdk = argv[++i];
        else if (!strcmp(argv[i], "--kernel-elf") && !ld_kernel_mode)
            ld_kernel_mode = 1;
        else if (!strcmp(argv[i], "--efi-loader") && i + 1 < argc && !efi_loader)
            efi_loader = argv[++i];
        else if (!strcmp(argv[i], "--kernel-payload") && i + 1 < argc && !kernel_payload)
            kernel_payload = argv[++i];
        else if (!strcmp(argv[i], "--boot-media") && i + 1 < argc && !boot_media)
            boot_media = argv[++i];
        else if (!strcmp(argv[i], "--help")) {
            puts("Usage: rcc-x64-ld INPUT.o/LIBRARY.a... -o PROGRAM [--metadata LEGACY.ssa]");
            puts("       rcc-x64-ld --kernel-elf KERNEL.o PLATFORM.o... -o kernel.elf");
            puts("       rcc-x64-ld --efi-loader loader.obj --kernel-payload kernel.elf -o "
                 "BOOTX64.EFI");
            puts("       rcc-x64-ld --boot-media BOOTX64.EFI -o boot.img");
            return 0;
        } else if (argv[i][0] == '-')
            ld_error("invalid option (use --metadata PROGRAM.ssa INPUT.o... -o PROGRAM)");
    }
    if (!output || (metadata && !strcmp(metadata, output)) || (metadata && sdk) ||
        (ld_kernel_mode && (metadata || sdk)))
        ld_error("distinct input and output paths are required");
    if (efi_loader || kernel_payload) {
        if (!efi_loader || !kernel_payload || sdk || metadata || ld_kernel_mode || boot_media ||
            !strcmp(output, efi_loader) || !strcmp(output, kernel_payload))
            ld_error("EFI mode requires distinct loader, kernel payload and output paths");
        for (i = 1; i < argc; i++) {
            if (!strcmp(argv[i], "-o") || !strcmp(argv[i], "--efi-loader") ||
                !strcmp(argv[i], "--kernel-payload"))
                i++;
            else
                ld_error("EFI mode does not accept other object inputs");
        }
        goto output;
    }
    if (boot_media) {
        if (sdk || metadata || ld_kernel_mode || !strcmp(output, boot_media))
            ld_error("boot media requires distinct EFI input and output paths");
        for (i = 1; i < argc; i++) {
            if (!strcmp(argv[i], "-o") || !strcmp(argv[i], "--boot-media"))
                i++;
            else
                ld_error("boot media mode does not accept object inputs");
        }
        goto output;
    }
    if (metadata)
        ld_metadata(metadata);
    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-o") || !strcmp(argv[i], "--metadata") || !strcmp(argv[i], "--sdk"))
            i++;
        else if (!strcmp(argv[i], "--kernel-elf"))
            continue;
        else {
            if (!strcmp(output, argv[i]))
                ld_error("output must not overwrite input");
            ld_input(argv[i]);
        }
    }
    if (!ld_objects.count)
        ld_error("no input objects");
    if (sdk)
        ld_sdk(sdk, output);
    if (!ld_kernel_mode)
        ld_modules(metadata != NULL);
    ld_link();
output:
    ld_path = output;
    pending = ld_memory(strlen(output) + 40);
    sprintf(pending, "%s.tmp.%u", output, (unsigned)getpid());
    fd = open(pending, O_CREAT | O_EXCL | O_WRONLY, 0600);
    if (fd < 0)
        ld_error("cannot create temporary output");
    atexit(cleanup);
    if (close(fd))
        ld_error("cannot close temporary output");
    f = fopen(pending, "wb");
    if (!f)
        ld_error("cannot open temporary output");
    if (boot_media)
        ld_boot_media(boot_media, f);
    else if (efi_loader)
        ld_efi(efi_loader, kernel_payload, f);
    else if (ld_kernel_mode)
        ld_kernel_write(f);
    else
        ld_write(f);
    if (fflush(f) || ferror(f) || fclose(f))
        ld_error("cannot finish executable");
    mask = umask(0);
    umask(mask);
    if (chmod(pending, (mode_t)(0777 & ~mask)) || rename(pending, output))
        ld_error("cannot publish executable");
    pending = NULL;
    return 0;
}
