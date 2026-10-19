//! Explicit native helper address table used by application binding stubs.
use std::ffi::c_void;
pub(super) fn resolve(name: &str, arity: usize) -> Option<usize> {
    if name
        .strip_prefix("rcc_native_ext_")
        .is_some_and(|service| !rcc_ir::native_image::application_service(service))
    {
        return None;
    }
    unsafe extern "C" {
        fn rcc_native_poll(ctx: *mut c_void) -> u64;
        fn rcc_native_load(ctx: *mut c_void, a0: u64, a1: u64) -> u64;
        fn rcc_native_store(ctx: *mut c_void, a0: u64, a1: u64, a2: u64) -> u64;
        fn rcc_native_gep(ctx: *mut c_void, a0: u64, a1: u64) -> u64;
        fn rcc_native_stack_mark(ctx: *mut c_void) -> u64;
        fn rcc_native_stack_alloc(ctx: *mut c_void, a0: u64) -> u64;
        fn rcc_native_stack_restore(ctx: *mut c_void, a0: u64) -> u64;
        fn rcc_native_register(ctx: *mut c_void, a0: u64, a1: u64) -> u64;
        fn rcc_native_function(ctx: *mut c_void, a0: u64, a1: u64) -> u64;
        fn rcc_native_process_frame(ctx: *mut c_void, a0: u64) -> u64;
        fn rcc_native_process_slot(ctx: *mut c_void, a0: u64) -> u64;
        fn rcc_native_process_pc(ctx: *mut c_void) -> u64;
        fn rcc_native_process_checkpoint(ctx: *mut c_void) -> u64;
        fn rcc_native_process_save(ctx: *mut c_void, a0: u64, a1: u64) -> u64;
        fn rcc_native_process_set_pc(ctx: *mut c_void, a0: u64) -> u64;
        fn rcc_native_process_arg(ctx: *mut c_void, a0: u64) -> u64;
        fn rcc_native_process_argument(ctx: *mut c_void, a0: u64, a1: u64) -> u64;
        fn rcc_native_process_call(ctx: *mut c_void, a0: u64, a1: u64, a2: u64) -> u64;
        fn rcc_native_process_return(ctx: *mut c_void, a0: u64) -> u64;
        fn rcc_native_process_request(ctx: *mut c_void, a0: u64, a1: u64, a2: u64, a3: u64) -> u64;
        fn rcc_native_process_bad_pc(ctx: *mut c_void) -> u64;
        fn rcc_native_ext_rcc_hosted_input(ctx: *mut c_void) -> u64;
        fn rcc_native_ext_rcc_hosted_idle(ctx: *mut c_void, a0: u64) -> u64;
        fn rcc_native_ext_abs(ctx: *mut c_void, a0: u64) -> u64;
        fn rcc_native_ext_labs(ctx: *mut c_void, a0: u64) -> u64;
        fn rcc_native_ext_llabs(ctx: *mut c_void, a0: u64) -> u64;
        fn rcc_native_ext_remove(ctx: *mut c_void, a0: u64) -> u64;
        fn rcc_native_ext_rename(ctx: *mut c_void, a0: u64, a1: u64) -> u64;
        fn rcc_native_ext_fopen(ctx: *mut c_void, a0: u64, a1: u64) -> u64;
        fn rcc_native_ext_fclose(ctx: *mut c_void, a0: u64) -> u64;
        fn rcc_native_ext_fread(ctx: *mut c_void, a0: u64, a1: u64, a2: u64, a3: u64) -> u64;
        fn rcc_native_ext_fwrite(ctx: *mut c_void, a0: u64, a1: u64, a2: u64, a3: u64) -> u64;
        fn rcc_native_ext_fgetc(ctx: *mut c_void, a0: u64) -> u64;
        fn rcc_native_ext_getc(ctx: *mut c_void, a0: u64) -> u64;
        fn rcc_native_ext_fseek(ctx: *mut c_void, a0: u64, a1: u64, a2: u64) -> u64;
        fn rcc_native_ext_ftell(ctx: *mut c_void, a0: u64) -> u64;
        fn rcc_native_ext_feof(ctx: *mut c_void, a0: u64) -> u64;
        fn rcc_native_ext_ferror(ctx: *mut c_void, a0: u64) -> u64;
        fn rcc_native_ext_fflush(ctx: *mut c_void, a0: u64) -> u64;
        fn rcc_native_ext_fputc(ctx: *mut c_void, a0: u64, a1: u64) -> u64;
        fn rcc_native_ext_putc(ctx: *mut c_void, a0: u64, a1: u64) -> u64;
        fn rcc_native_ext_fputs(ctx: *mut c_void, a0: u64, a1: u64) -> u64;
        fn rcc_native_ext_putchar(ctx: *mut c_void, a0: u64) -> u64;
        fn rcc_native_ext_puts(ctx: *mut c_void, a0: u64) -> u64;
        fn rcc_native_ext_getchar(ctx: *mut c_void) -> u64;
        fn rcc_native_ext_malloc(ctx: *mut c_void, a0: u64) -> u64;
        fn rcc_native_ext_free(ctx: *mut c_void, a0: u64) -> u64;
        fn rcc_native_ext_calloc(ctx: *mut c_void, a0: u64, a1: u64) -> u64;
        fn rcc_native_ext_realloc(ctx: *mut c_void, a0: u64, a1: u64) -> u64;
        fn rcc_native_ext_memset(ctx: *mut c_void, a0: u64, a1: u64, a2: u64) -> u64;
        fn rcc_native_ext_memcpy(ctx: *mut c_void, a0: u64, a1: u64, a2: u64) -> u64;
        fn rcc_native_ext_memmove(ctx: *mut c_void, a0: u64, a1: u64, a2: u64) -> u64;
        fn rcc_native_ext_strlen(ctx: *mut c_void, a0: u64) -> u64;
        fn rcc_native_ext_strcmp(ctx: *mut c_void, a0: u64, a1: u64) -> u64;
        fn rcc_native_ext_strncmp(ctx: *mut c_void, a0: u64, a1: u64, a2: u64) -> u64;
        fn rcc_native_ext_strcpy(ctx: *mut c_void, a0: u64, a1: u64) -> u64;
        fn rcc_native_ext_strncpy(ctx: *mut c_void, a0: u64, a1: u64, a2: u64) -> u64;
        fn rcc_native_ext_strcat(ctx: *mut c_void, a0: u64, a1: u64) -> u64;
        fn rcc_native_ext_memcmp(ctx: *mut c_void, a0: u64, a1: u64, a2: u64) -> u64;
        fn rcc_native_ext_memchr(ctx: *mut c_void, a0: u64, a1: u64, a2: u64) -> u64;
        fn rcc_native_ext_strchr(ctx: *mut c_void, a0: u64, a1: u64) -> u64;
        fn rcc_native_ext_strrchr(ctx: *mut c_void, a0: u64, a1: u64) -> u64;
        fn rcc_native_ext_mmio_read(ctx: *mut c_void, a0: u64) -> u64;
        fn rcc_native_ext_mmio_write(ctx: *mut c_void, a0: u64, a1: u64) -> u64;
        fn rcc_native_ext_rfl_halt(ctx: *mut c_void) -> u64;
        fn rcc_native_ext___rcc_host_math(ctx: *mut c_void, op: u64, a: u64, b: u64) -> u64;
        fn rcc_native_ext___rfl_host_sin(ctx: *mut c_void, a0: u64) -> u64;
        fn rcc_native_ext___rfl_host_cos(ctx: *mut c_void, a0: u64) -> u64;
        fn rcc_native_ext___rcc_f64_floor(ctx: *mut c_void, a0: u64) -> u64;
        fn rcc_native_ext___rcc_f64_sqrt(ctx: *mut c_void, a0: u64) -> u64;
    }
    Some(match (name, arity) {
        ("rcc_native_poll", 0) => rcc_native_poll as *const () as usize,
        ("rcc_native_load", 2) => rcc_native_load as *const () as usize,
        ("rcc_native_store", 3) => rcc_native_store as *const () as usize,
        ("rcc_native_gep", 2) => rcc_native_gep as *const () as usize,
        ("rcc_native_stack_mark", 0) => rcc_native_stack_mark as *const () as usize,
        ("rcc_native_stack_alloc", 1) => rcc_native_stack_alloc as *const () as usize,
        ("rcc_native_stack_restore", 1) => rcc_native_stack_restore as *const () as usize,
        ("rcc_native_register", 2) => rcc_native_register as *const () as usize,
        ("rcc_native_function", 2) => rcc_native_function as *const () as usize,
        ("rcc_native_process_frame", 1) => rcc_native_process_frame as *const () as usize,
        ("rcc_native_process_slot", 1) => rcc_native_process_slot as *const () as usize,
        ("rcc_native_process_pc", 0) => rcc_native_process_pc as *const () as usize,
        ("rcc_native_process_checkpoint", 0) => rcc_native_process_checkpoint as *const () as usize,
        ("rcc_native_process_save", 2) => rcc_native_process_save as *const () as usize,
        ("rcc_native_process_set_pc", 1) => rcc_native_process_set_pc as *const () as usize,
        ("rcc_native_process_arg", 1) => rcc_native_process_arg as *const () as usize,
        ("rcc_native_process_argument", 2) => rcc_native_process_argument as *const () as usize,
        ("rcc_native_process_call", 3) => rcc_native_process_call as *const () as usize,
        ("rcc_native_process_return", 1) => rcc_native_process_return as *const () as usize,
        ("rcc_native_process_request", 4) => rcc_native_process_request as *const () as usize,
        ("rcc_native_process_bad_pc", 0) => rcc_native_process_bad_pc as *const () as usize,
        ("rcc_native_ext_rcc_hosted_input", 0) => {
            rcc_native_ext_rcc_hosted_input as *const () as usize
        }
        ("rcc_native_ext_rcc_hosted_idle", 1) => {
            rcc_native_ext_rcc_hosted_idle as *const () as usize
        }
        ("rcc_native_ext_abs", 1) => rcc_native_ext_abs as *const () as usize,
        ("rcc_native_ext_labs", 1) => rcc_native_ext_labs as *const () as usize,
        ("rcc_native_ext_llabs", 1) => rcc_native_ext_llabs as *const () as usize,
        ("rcc_native_ext_remove", 1) => rcc_native_ext_remove as *const () as usize,
        ("rcc_native_ext_rename", 2) => rcc_native_ext_rename as *const () as usize,
        ("rcc_native_ext_fopen", 2) => rcc_native_ext_fopen as *const () as usize,
        ("rcc_native_ext_fclose", 1) => rcc_native_ext_fclose as *const () as usize,
        ("rcc_native_ext_fread", 4) => rcc_native_ext_fread as *const () as usize,
        ("rcc_native_ext_fwrite", 4) => rcc_native_ext_fwrite as *const () as usize,
        ("rcc_native_ext_fgetc", 1) => rcc_native_ext_fgetc as *const () as usize,
        ("rcc_native_ext_getc", 1) => rcc_native_ext_getc as *const () as usize,
        ("rcc_native_ext_fseek", 3) => rcc_native_ext_fseek as *const () as usize,
        ("rcc_native_ext_ftell", 1) => rcc_native_ext_ftell as *const () as usize,
        ("rcc_native_ext_feof", 1) => rcc_native_ext_feof as *const () as usize,
        ("rcc_native_ext_ferror", 1) => rcc_native_ext_ferror as *const () as usize,
        ("rcc_native_ext_fflush", 1) => rcc_native_ext_fflush as *const () as usize,
        ("rcc_native_ext_fputc", 2) => rcc_native_ext_fputc as *const () as usize,
        ("rcc_native_ext_putc", 2) => rcc_native_ext_putc as *const () as usize,
        ("rcc_native_ext_fputs", 2) => rcc_native_ext_fputs as *const () as usize,
        ("rcc_native_ext_putchar", 1) => rcc_native_ext_putchar as *const () as usize,
        ("rcc_native_ext_puts", 1) => rcc_native_ext_puts as *const () as usize,
        ("rcc_native_ext_getchar", 0) => rcc_native_ext_getchar as *const () as usize,
        ("rcc_native_ext_malloc", 1) => rcc_native_ext_malloc as *const () as usize,
        ("rcc_native_ext_free", 1) => rcc_native_ext_free as *const () as usize,
        ("rcc_native_ext_calloc", 2) => rcc_native_ext_calloc as *const () as usize,
        ("rcc_native_ext_realloc", 2) => rcc_native_ext_realloc as *const () as usize,
        ("rcc_native_ext_memset", 3) => rcc_native_ext_memset as *const () as usize,
        ("rcc_native_ext_memcpy", 3) => rcc_native_ext_memcpy as *const () as usize,
        ("rcc_native_ext_memmove", 3) => rcc_native_ext_memmove as *const () as usize,
        ("rcc_native_ext_strlen", 1) => rcc_native_ext_strlen as *const () as usize,
        ("rcc_native_ext_strcmp", 2) => rcc_native_ext_strcmp as *const () as usize,
        ("rcc_native_ext_strncmp", 3) => rcc_native_ext_strncmp as *const () as usize,
        ("rcc_native_ext_strcpy", 2) => rcc_native_ext_strcpy as *const () as usize,
        ("rcc_native_ext_strncpy", 3) => rcc_native_ext_strncpy as *const () as usize,
        ("rcc_native_ext_strcat", 2) => rcc_native_ext_strcat as *const () as usize,
        ("rcc_native_ext_memcmp", 3) => rcc_native_ext_memcmp as *const () as usize,
        ("rcc_native_ext_memchr", 3) => rcc_native_ext_memchr as *const () as usize,
        ("rcc_native_ext_strchr", 2) => rcc_native_ext_strchr as *const () as usize,
        ("rcc_native_ext_strrchr", 2) => rcc_native_ext_strrchr as *const () as usize,
        ("rcc_native_ext_mmio_read", 1) => rcc_native_ext_mmio_read as *const () as usize,
        ("rcc_native_ext_mmio_write", 2) => rcc_native_ext_mmio_write as *const () as usize,
        ("rcc_native_ext_rfl_halt", 0) => rcc_native_ext_rfl_halt as *const () as usize,
        ("rcc_native_ext___rcc_host_math", 3) => {
            rcc_native_ext___rcc_host_math as *const () as usize
        }
        ("rcc_native_ext___rfl_host_sin", 1) => rcc_native_ext___rfl_host_sin as *const () as usize,
        ("rcc_native_ext___rfl_host_cos", 1) => rcc_native_ext___rfl_host_cos as *const () as usize,
        ("rcc_native_ext___rcc_f64_floor", 1) => {
            rcc_native_ext___rcc_f64_floor as *const () as usize
        }
        ("rcc_native_ext___rcc_f64_sqrt", 1) => rcc_native_ext___rcc_f64_sqrt as *const () as usize,
        ("rcc_native_image_token", 1) => {
            super::images::rcc_native_image_token as *const () as usize
        }
        ("rcc_native_image_global", 1) => {
            super::images::rcc_native_image_global as *const () as usize
        }
        _ => return None,
    })
}
