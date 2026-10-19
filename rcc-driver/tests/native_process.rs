//! Real compile/link/execute coverage of clonable native C continuations.
use std::{
    path::{Path, PathBuf},
    process::Command,
};

struct Fixture(PathBuf);
static NEXT_FIXTURE: std::sync::atomic::AtomicU64 = std::sync::atomic::AtomicU64::new(0);

#[test]
fn hosted_kernel_selects_contexts_and_exec_resets_only_private_globals() {
    let fixture = Fixture::new();
    let sources = [
        (
            "kernel",
            r#"
            #include <native_hosted.h>
            struct State { int padding; int counter; };
            struct State states[2];
            int shared;
            extern int guest(void);
            void pause_kernel(void) { rcc_hosted_idle(0); }
            int main(void) {
                for (int i = 0; i < 3; i++) {
                    states[1].counter = i;
                    pause_kernel();
                    if (states[1].counter != i) return 94;
                }
                return guest();
            }
        "#,
        ),
        (
            "application",
            r#"
        #include <native_hosted.h>
        #include <stdlib.h>
        extern int shared;
        static int global = 7;
        void replacement(unsigned long argument) {
            if (argument != 23 || global != 7 || shared != 2) exit(90);
            rcc_hosted_drop_switch(0);
            exit(91);
        }
        int guest(void) {
            int local = 11;
            int child = rcc_hosted_clone(1);
            if (child == 0) {
                global = 99; local = 22; shared = 1;
                rcc_hosted_switch(0);
                if (global != 99 || local != 22 || shared != 2) exit(92);
                rcc_hosted_exec(replacement, 23);
                return 93;
            }
            if (child != 1) return 1;
            shared = 10;
            rcc_hosted_switch(1);
            if (global != 7 || local != 11 || shared != 1) return 2;
            shared = 2;
            rcc_hosted_switch(1);
            if (global != 7 || shared != 2) return 3;
            return 42;
        }
        "#,
        ),
    ];
    for (name, source) in sources {
        let input = fixture.0.join(format!("{name}.c"));
        std::fs::write(&input, source).unwrap();
        let result = Command::new(env!("CARGO_BIN_EXE_rcc"))
            .args(["compile", "--target", "native", "--emit-ir", "-I"])
            .arg(Path::new(env!("CARGO_MANIFEST_DIR")).join("../runtime/include"))
            .arg(&input)
            .output()
            .unwrap();
        assert!(
            result.status.success(),
            "{}",
            String::from_utf8_lossy(&result.stderr)
        );
        std::fs::write(fixture.0.join(format!("{name}.ir")), result.stdout).unwrap();
    }
    let linked = Command::new(env!("CARGO_BIN_EXE_rcc"))
        .args(["link-native", "--kernel"])
        .arg(fixture.0.join("kernel.ir"))
        .arg(fixture.0.join("application.ir"))
        .arg("-o")
        .arg(fixture.0.join("process"))
        .output()
        .unwrap();
    assert!(
        linked.status.success(),
        "{}",
        String::from_utf8_lossy(&linked.stderr)
    );
    fixture.execute(&[], 42);
}
impl Fixture {
    fn new() -> Self {
        let serial = std::time::SystemTime::now()
            .duration_since(std::time::UNIX_EPOCH)
            .unwrap()
            .as_nanos();
        let sequence = NEXT_FIXTURE.fetch_add(1, std::sync::atomic::Ordering::Relaxed);
        let dir = std::env::temp_dir().join(format!(
            "rcc-process-{}-{serial}-{sequence}",
            std::process::id()
        ));
        std::fs::create_dir(&dir).unwrap();
        Self(dir)
    }
    fn compile(&self, source: &str) -> std::process::Output {
        let input = self.0.join("process.c");
        std::fs::write(&input, source).unwrap();
        Command::new(env!("CARGO_BIN_EXE_rcc"))
            .args(["compile", "--target", "native", "-I"])
            .arg(Path::new(env!("CARGO_MANIFEST_DIR")).join("../runtime/include"))
            .arg(input)
            .arg("-o")
            .arg(self.0.join("process"))
            .output()
            .unwrap()
    }
    fn run(&self, source: &str, expected: i32) -> std::process::Output {
        self.run_limit(source, expected, 16)
    }
    fn run_limit(&self, source: &str, expected: i32, limit: usize) -> std::process::Output {
        let compile = self.compile(source);
        assert!(
            compile.status.success(),
            "{}",
            String::from_utf8_lossy(&compile.stderr)
        );
        self.execute_limit(&[], expected, limit)
    }
    fn execute(&self, args: &[&str], expected: i32) -> std::process::Output {
        self.execute_limit(args, expected, 16)
    }
    fn execute_limit(&self, args: &[&str], expected: i32, limit: usize) -> std::process::Output {
        let run = Command::new(self.0.join("process"))
            .args(args)
            .env("RCC_NATIVE_HEADLESS", "1")
            .env("RCC_NATIVE_NET", "off")
            .env("RCC_NATIVE_TIMEOUT", "10")
            .env("RCC_NATIVE_MAX_PROCESSES", limit.to_string())
            .output()
            .unwrap();
        assert_eq!(
            run.status.code(),
            Some(expected),
            "{}",
            String::from_utf8_lossy(&run.stderr)
        );
        run
    }
}

#[test]
fn shared_kernel_state_and_allocator_coexist_with_private_globals_and_bulk_memory() {
    Fixture::new().run(
        r#"
        #include <native_process.h>
        #include <stdlib.h>
        #include <string.h>
        struct Kernel { int counter; void *child_allocation; int status; };
        static int global = 7;
        int main(void) {
            struct Kernel *k = rcc_process_kernel_alloc(sizeof(struct Kernel));
            char *private = malloc(1800UL);
            if (!k || !private) return 1;
            if ((unsigned long)private >= 0xf00000UL || (unsigned long)k < 0xf00000UL) return 2;
            memset(private, 42, 1800UL);
            k->counter = 10; k->child_allocation = 0;
            int pid = rcc_process_fork();
            if (pid == 0) {
                rcc_process_yield();
                if (private[0] != 42) return 92;
                k->counter += 5;
                k->child_allocation = rcc_process_kernel_alloc(2000UL);
                if (!k->child_allocation) return 90;
                memset(k->child_allocation, 31, 2000UL);
                global = 99;
                memset(private+500, 77, 1100UL);
                memmove(private+510, private+500, 1000UL);
                private = realloc(private, 2400UL);
                if (!private || private[510] != 77) return 91;
                strcpy(private,"child");
                free(private);
                return 23;
            }
            private[0] = 43;
            if (pid < 0 || rcc_process_wait(pid,&k->status) != pid || k->status != 23) return 3;
            if (k->counter != 15 || !k->child_allocation || global != 7) return 4;
            char *shared = k->child_allocation;
            for (int i=0; i<2000; i++) if (shared[i] != 31) return 5;
            for (int i=0; i<1800; i++) if (private[i] != (i == 0 ? 43 : 42)) return 6;
            void *old = k->child_allocation;
            rcc_process_kernel_free(old);
            void *again = rcc_process_kernel_alloc(2000UL);
            if (again != old) return 7;
            rcc_process_kernel_free(again);
            rcc_process_kernel_free(k);
            free(private);
            return 42;
        }
    "#,
        42,
    );
}

#[test]
fn many_unreaped_children_share_memory_and_capacity_is_configurable() {
    Fixture::new().run_limit(
        r#"
        #include <native_process.h>
        int main(void) {
            int children = 0, status;
            int *gate = rcc_process_kernel_alloc(sizeof(int));
            if (!gate) return 3;
            *gate = 0;
            for (int i=0; i<128; i++) {
                int pid = rcc_process_fork();
                if (pid == 0) {
                    while (*gate == 0) rcc_process_yield();
                    return 11;
                }
                if (pid == -1) break;
                children++;
            }
            if (children != 63) return 1;
            *gate = 1;
            for (int i=0; i<children; i++)
                if (rcc_process_wait(-1,&status) < 0 || status != 11) return 2;
            rcc_process_kernel_free(gate);
            return 42;
        }
    "#,
        42,
        64,
    );
}

#[test]
fn invalid_process_limit_is_reported() {
    let result = Fixture::new().run_limit(
        "#include <native_process.h>\nint main(void) {rcc_process_yield();return 0;}",
        1,
        0,
    );
    assert!(String::from_utf8_lossy(&result.stderr)
        .contains("RCC_NATIVE_MAX_PROCESSES must be between 1 and 32767"));
}

#[test]
fn file_reads_detach_only_the_reading_process_buffer() {
    let f = Fixture::new();
    let input = f.0.join("input.bin");
    std::fs::write(&input, vec![77; 1300]).unwrap();
    let source = r#"
        #include <native_process.h>
        #include <stdlib.h>
        #include <string.h>
        typedef struct File File;
        extern File *fopen(const char *, const char *);
        extern size_t fread(void *,size_t,size_t,File *);
        extern int fclose(File *);
        int main(int argc,char **argv) {
            if (argc != 2) return 1;
            char *buffer = malloc(1300UL);
            memset(buffer,42,1300UL);
            File *file = fopen(argv[1],"rb");
            if (!file) return 2;
            int pid = rcc_process_fork();
            if (pid == 0) {
                if (fread(buffer,1UL,1300UL,file) != 1300UL) return 90;
                for (int i=0; i<1300; i++) if (buffer[i] != 77) return 91;
                fclose(file); return 23;
            }
            int status;
            if (pid < 0 || rcc_process_wait(pid,&status) != pid || status != 23) return 3;
            for (int i=0; i<1300; i++) if (buffer[i] != 42) return 4;
            free(buffer); return 42;
        }
    "#;
    let compile = f.compile(source);
    assert!(
        compile.status.success(),
        "{}",
        String::from_utf8_lossy(&compile.stderr)
    );
    f.execute(&[input.to_str().unwrap()], 42);
}

#[test]
fn whole_program_linking_preserves_main_arguments_in_both_processes() {
    let f = Fixture::new();
    let sources = [
        ("worker", "#include <native_process.h>\nint worker(void) { rcc_process_yield(); return rcc_process_fork(); }"),
        ("main", r#"
            #include <native_process.h>
            extern int worker(void);
            int main(int argc, char **argv) {
                if (argc != 2 || argv[2] != 0 || argv[1][0] != 'h') return 1;
                int pid = worker();
                if (pid == 0) { argv[1][0] = 'z'; return 17; }
                int status;
                if (pid < 0 || rcc_process_wait(pid,&status) != pid) return 2;
                return status == 17 && argv[1][0] == 'h' ? 42 : 3;
            }
        "#),
    ];
    let mut units = Vec::new();
    for (name, source) in sources {
        let c = f.0.join(format!("{name}.c"));
        std::fs::write(&c, source).unwrap();
        let output = Command::new(env!("CARGO_BIN_EXE_rcc"))
            .args(["compile", "--target", "native", "--emit-ir", "-I"])
            .arg(Path::new(env!("CARGO_MANIFEST_DIR")).join("../runtime/include"))
            .arg(c)
            .output()
            .unwrap();
        assert!(
            output.status.success(),
            "{}",
            String::from_utf8_lossy(&output.stderr)
        );
        let ir = f.0.join(format!("{name}.ir"));
        std::fs::write(&ir, output.stdout).unwrap();
        units.push(ir);
    }
    let link = Command::new(env!("CARGO_BIN_EXE_rcc"))
        .arg("link-native")
        .args(units)
        .arg("-o")
        .arg(f.0.join("process"))
        .output()
        .unwrap();
    assert!(
        link.status.success(),
        "{}",
        String::from_utf8_lossy(&link.stderr)
    );
    f.execute(&["hello"], 42);
}

#[test]
fn deep_continuations_and_orphans_finish_without_a_saved_host_stack() {
    let run = Fixture::new().run(
        r#"
        #include <native_process.h>
        extern int puts(const char *);
        int descend(int n) {
            volatile int local = n ^ 123;
            int pid;
            if (n) pid = descend(n-1);
            else pid = rcc_process_fork();
            if (local != (n ^ 123)) rcc_process_exit(90);
            return pid;
        }
        int main(void) {
            int pid = descend(4096);
            if (pid == 0) {
                rcc_process_yield();
                puts("orphan continuation finished");
                return 7;
            }
            return pid > 1 ? 42 : 1;
        }
    "#,
        42,
    );
    assert_eq!(run.stdout, b"orphan continuation finished\n");
}
impl Drop for Fixture {
    fn drop(&mut self) {
        let _ = std::fs::remove_dir_all(&self.0);
    }
}

#[test]
fn nested_fork_restores_live_values_and_private_stack_global_heap() {
    let run = Fixture::new().run(include_str!("../../rcc-native/examples/processes.c"), 0);
    assert_eq!(
        String::from_utf8(run.stdout).unwrap(),
        concat!(
            "native: before fork\n",
            "native: child resumed through three C frames\n",
            "native: parent reaped child; stack, globals and heap are independent\n"
        )
    );
}

#[test]
fn indirect_variadic_calls_resume_with_live_arguments_and_float_values() {
    Fixture::new().run(
        r#"
        #include <native_process.h>
        #include <stdarg.h>
        int worker(int n, ...) {
            va_list a; va_start(a,n);
            int first = va_arg(a,int);
            double d = va_arg(a,double);
            int pid = rcc_process_fork();
            rcc_process_yield();
            long last = va_arg(a,long);
            va_end(a);
            if (first != 10 || d != 1.5 || last != 20L) rcc_process_exit(99);
            return n + first + (int)last + pid;
        }
        int invoke(int (*fn)(int,...)) { return fn(2,10,1.5,20L); }
        int main(void) {
            int result = invoke(worker);
            if (rcc_process_getpid() != 1) rcc_process_exit(result == 32 ? 19 : 98);
            int status;
            int child = rcc_process_wait(-1,&status);
            return child > 1 && result == 32 + child && status == 19 ? 42 : 1;
        }
    "#,
        42,
    );
}

#[test]
fn multiple_children_grandchildren_phi_loops_and_wait_errors() {
    Fixture::new().run(r#"
        #include <native_process.h>
        #include <stdlib.h>
        int main(void) {
            int status = 77, total = 0;
            if (rcc_process_wait(-1,&status) != -1 || status != 77) return 1;
            if (rcc_process_wait(1,0) != -1) return 2;
            for (int i = 0; i < 3; i++) {
                int kept = 100+i;
                int pid = rcc_process_fork();
                if (pid < 0) return 3;
                if (pid == 0) {
                    if (i == 1) {
                        int grandchild = rcc_process_fork();
                        if (grandchild == 0) exit(21);
                        if (rcc_process_wait(grandchild,&status) != grandchild || status != 21) exit(90);
                    }
                    rcc_process_yield();
                    exit(kept - 90);
                }
                if (kept != 100+i) return 4;
            }
            for (int i = 0; i < 3; i++) {
                if (rcc_process_wait(-1,&status) <= 1) return 5;
                total += status;
            }
            if (rcc_process_wait(-1,0) != -1) return 6;
            return total == 33 ? 42 : 7;
        }
    "#, 42);
}

#[test]
fn process_limit_fails_cleanly_and_reaped_slots_are_reusable() {
    Fixture::new().run(
        r#"
        #include <native_process.h>
        int main(void) {
            int children = 0, status;
            for (int i = 0; i < 30; i++) {
                int pid = rcc_process_fork();
                if (pid == 0) return 12;
                if (pid == -1) break;
                children++;
            }
            if (children != 15) return 1;
            for (int i = 0; i < children; i++)
                if (rcc_process_wait(-1,&status) < 0 || status != 12) return 2;
            int pid = rcc_process_fork();
            if (pid == 0) return 13;
            if (pid < 0 || rcc_process_wait(pid,&status) != pid || status != 13) return 3;
            return 42;
        }
    "#,
        42,
    );
}

#[test]
fn runtime_function_pointers_are_rejected_in_resumable_programs() {
    let result = Fixture::new().compile(
        r#"
        #include <native_process.h>
        extern int puts(const char *);
        int invoke(int (*fn)(const char *)) { rcc_process_yield(); return fn("hi"); }
        int main(void) { return invoke(puts); }
    "#,
    );
    assert!(!result.status.success());
    assert!(
        String::from_utf8_lossy(&result.stderr)
            .contains("cannot take the address of runtime binding puts"),
        "{}",
        String::from_utf8_lossy(&result.stderr)
    );
}

#[test]
fn nonlocal_jumps_restore_native_continuations_across_fork() {
    Fixture::new().run(
        r#"
        #include <native_process.h>
        typedef unsigned jmp_buf[10];
        extern int setjmp(jmp_buf);
        extern void longjmp(jmp_buf, int);
        jmp_buf target;
        volatile int changed;
        void descend(int n) { if (n) descend(n-1); longjmp(target, -9); }
        int main(void) {
            int value = setjmp(target);
            if (!value) { changed=23; descend(8); }
            if (value!=-9 || changed!=23) return 1;
            value = setjmp(target);
            if (!value) longjmp(target, 0);
            if (value!=1) return 2;
            value = setjmp(target);
            if (value) return value==17 ? 17 : 3;
            int pid=rcc_process_fork();
            if (!pid) longjmp(target,17);
            int status;
            if(rcc_process_wait(pid,&status)!=pid || status!=17) return 4;
            return 42;
        }
    "#,
        42,
    );
}

#[test]
fn nonlocal_jump_to_returned_activation_is_rejected() {
    let fixture = Fixture::new();
    let compiled = fixture.compile(
        r#"
        #include <native_process.h>
        typedef unsigned jmp_buf[10];
        extern int setjmp(jmp_buf);
        extern void longjmp(jmp_buf, int);
        jmp_buf target;
        void expired(void) { setjmp(target); }
        int main(void) {
            rcc_process_getpid();
            expired();
            longjmp(target, 3);
            return 42;
        }
    "#,
    );
    assert!(
        compiled.status.success(),
        "{}",
        String::from_utf8_lossy(&compiled.stderr)
    );
    let result = fixture.execute(&[], 1);
    assert!(String::from_utf8_lossy(&result.stderr).contains("expired native jump buffer"));
}

#[test]
fn computation_loops_schedule_children_without_explicit_yields() {
    Fixture::new().run(
        r#"
        #include <native_process.h>
        int main(void) {
            volatile unsigned *flag=rcc_process_kernel_alloc(1);
            if(!flag)return 1;
            *flag=0;
            int child=rcc_process_fork();
            if(child<0)return 2;
            if(!child) { *flag=1; rcc_process_exit(17); }
            unsigned long a=11,b=7;
            while(!*flag) {
                a=a*1664525UL+1013904223UL;
                b=b*1664525UL+1013904223UL;
            }
            int status;
            if(rcc_process_wait(child,&status)!=child || status!=17)return 3;
            rcc_process_kernel_free((void *)flag);
            return a==b ? 4 : 42;
        }
        "#,
        42,
    );
}

#[test]
fn direct_helpers_and_resumable_callbacks_keep_their_abi_across_fork() {
    let source = concat!(
        "#include <native_process.h>\n",
        "static volatile unsigned long calls;\n",
        "unsigned long helper(unsigned long n) { ++calls; return n * 65537UL + 7; }\n",
        "static unsigned long (*volatile callback)(unsigned long) = helper;\n",
        "int main(void) {\n",
        "if(helper(3)!=196618UL || callback(5)!=327692UL || calls!=2) return 1;\n",
        "int pid=rcc_process_fork(); int status;\n",
        "if(!pid) { if(helper(9)!=589840UL || callback(11)!=720914UL || calls!=4) return 90; return 17; }\n",
        "if(rcc_process_wait(pid,&status)!=pid || status!=17 || calls!=2) return 2;\n",
        "return callback(1)==65544UL && calls==3 ? 42 : 3; }\n"
    );
    Fixture::new().run(source, 42);
}

#[test]
fn nonsuspending_variadic_helpers_have_both_direct_and_token_entry_points() {
    Fixture::new().run(r#"
        #include <stdarg.h>
        #include <native_process.h>
        int sum(int n, ...) {
            va_list a; va_start(a,n);
            long first=va_arg(a,long); double second=va_arg(a,double);
            va_end(a); return n+(int)first+(int)second;
        }
        static int (*volatile indirect)(int,...)=sum;
        int main(void) {
            rcc_process_yield();
            if(sum(3,20L,1.5)!=24 || indirect(4,30L,2.5)!=36)return 1;
            return 42;
        }
    "#, 42);
}
