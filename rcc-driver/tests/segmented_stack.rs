//! Compile real C units, assemble/link their runtime, and execute on RVM.
use ripple_asm::{AssemblerOptions, Linker, ObjectFile, Register, RippleAssembler};
use rvm::{vm::VMState, Instr, VM};
use std::{
    fs,
    path::Path,
    process::Command,
    sync::{
        atomic::{AtomicU64, Ordering},
        OnceLock,
    },
};

const BANK: u16 = 8192;
static SERIAL: AtomicU64 = AtomicU64::new(0);
static RUNTIME: OnceLock<Vec<ObjectFile>> = OnceLock::new();

fn assemble(source: &str) -> ObjectFile {
    let mut options = AssemblerOptions::default();
    options.bank_size = BANK;
    options.max_immediate = 65535;
    options.memory_offset = 0;
    RippleAssembler::new(options)
        .assemble(source)
        .unwrap_or_else(|e| panic!("{e:?}"))
}

fn compile(name: &str, source: &str, segmented: bool) -> ObjectFile {
    let root = std::env::temp_dir().join(format!(
        "rcc-stack-{}-{}",
        std::process::id(),
        SERIAL.fetch_add(1, Ordering::Relaxed)
    ));
    fs::create_dir_all(&root).unwrap();
    let input = root.join(format!("{name}.c"));
    let output = root.join("out.asm");
    fs::write(&input, source).unwrap();
    let mut command = Command::new(env!("CARGO_BIN_EXE_rcc"));
    command
        .arg("compile")
        .arg(&input)
        .arg("-o")
        .arg(&output)
        .arg("--bank-size")
        .arg(BANK.to_string())
        .arg("-D")
        .arg(format!("BANK_SIZE={BANK}"))
        .arg("-I")
        .arg(Path::new(env!("CARGO_MANIFEST_DIR")).join("../runtime/include"));
    if segmented {
        command.arg("--segmented-stack");
    }
    let result = command.output().unwrap();
    assert!(
        result.status.success(),
        "{name}: {}",
        String::from_utf8_lossy(&result.stderr)
    );
    let assembly = fs::read_to_string(output).unwrap();
    let _ = fs::remove_dir_all(root);
    assemble(&assembly)
}

fn runtime() -> &'static Vec<ObjectFile> {
    RUNTIME.get_or_init(|| {
        let root = Path::new(env!("CARGO_MANIFEST_DIR")).join("../runtime");
        let mut objects = vec![assemble(
            &fs::read_to_string(root.join("crt0.asm")).unwrap(),
        )];
        for (name, segmented) in [
            ("stack", false),
            ("irq", false),
            ("mmio", true),
            ("malloc", true),
            ("string", true),
        ] {
            objects.push(compile(
                name,
                &fs::read_to_string(root.join(format!("{name}.c"))).unwrap(),
                segmented,
            ));
        }
        objects.push(assemble(
            &fs::read_to_string(root.join("stack_segments.asm.in"))
                .unwrap()
                .replace("@LIMIT@", &(BANK - 10).to_string()),
        ));
        objects
    })
}

fn run(source: &str, expected: u16) {
    let program = compile("program", source, true);
    for (jit, interrupts) in [(false, false), (true, false), (true, true)] {
        let mut objects = runtime().clone();
        objects.push(program.clone());
        if interrupts {
            let root = Path::new(env!("CARGO_MANIFEST_DIR")).join("../runtime/crt0.asm");
            let setup = "LI T0, 4\nCSRW T0, 5\nLI T0, 1\nCSRW T0, 6\n\
                LI T0, %bank(stack_test_irq)\nLI T1, 24\nSTORE T0, R0, T1\n\
                LI T0, %offset(stack_test_irq)\nLI T1, 25\nSTORE T0, R0, T1\n\
                LI T0, 1009\nCSRW T0, 3\nLI T0, 2\nLI T1, 22\nSTORE T0, R0, T1\nCALL main";
            objects[0] = assemble(
                &fs::read_to_string(root)
                    .unwrap()
                    .replace("CALL main", setup),
            );
            let mut handler = "stack_test_irq:\n".to_string();
            let saved: Vec<_> = (3..=27).chain([31]).collect();
            for r in &saved {
                handler.push_str(&format!("STORE R{r}, SB, SP\nADDI SP, SP, 1\n"));
            }
            handler.push_str("LI T0, 100\nLOAD T1, SB, T0\nADDI T1, T1, 1\nSTORE T1, SB, T0\nLI T0, 23\nLI T1, 1\nSTORE T1, R0, T0\n");
            for r in saved.iter().rev() {
                handler.push_str(&format!("ADDI SP, SP, -1\nLOAD R{r}, SB, SP\n"));
            }
            handler.push_str("IRET\n");
            objects.push(assemble(&handler));
        }
        let linked = Linker::new(BANK)
            .link(objects)
            .unwrap_or_else(|e| panic!("{e:?}"));
        let disk = std::env::temp_dir().join(format!(
            "rcc-stack-disk-{}-{}",
            std::process::id(),
            SERIAL.fetch_add(1, Ordering::Relaxed)
        ));
        let mut vm = VM::with_options(BANK, usize::from(BANK) * 256, Some(disk.clone()));
        vm.instructions = linked
            .instructions
            .iter()
            .map(|i| Instr::new(i.opcode, i.opcode, i.word1, i.word2, i.word3))
            .collect();
        vm.state = VMState::Running;
        vm.host_input_enabled = false;
        vm.jit_enabled = jit;
        vm.memory[usize::from(BANK) * 3..usize::from(BANK) * 4].fill(0x5a5a);
        // The C initializer dispatcher creates the data; these fixtures have no
        // legacy bank-zero data to copy from the linked image.
        vm.registers[Register::Pc as usize] = linked.entry_point as u16;
        for _ in 0..200_000 {
            vm.run_batch(1024).unwrap();
            if !matches!(vm.state, VMState::Running) {
                break;
            }
        }
        assert!(
            matches!(vm.state, VMState::Halted),
            "did not terminate: {:?}",
            vm.state
        );
        assert_eq!(
            vm.registers[Register::Rv0 as usize],
            expected,
            "jit={jit}, output={:?}",
            vm.output_buffer
        );
        if interrupts {
            assert_ne!(
                vm.memory[usize::from(BANK) * 4 + 100],
                0,
                "timer never interrupted C execution"
            );
        }
        assert!(
            vm.memory[usize::from(BANK) * 3..usize::from(BANK) * 4]
                .iter()
                .all(|&v| v == 0x5a5a),
            "adjacent bank was corrupted"
        );
        if expected != 125 {
            assert_eq!(
                vm.registers[Register::Sb as usize],
                2,
                "root stack was not restored"
            );
            assert_eq!(
                vm.registers[Register::Sp as usize],
                1,
                "root SP was not restored"
            );
        }
        drop(vm);
        let _ = fs::remove_file(disk);
    }
}

#[test]
fn deep_calls_preserve_ancestor_pointers_stack_arguments_and_reuse_segments() {
    run(
        r#"
        #include <stack.h>
        #include <irq.h>
        int *ancestor;
        int walk(int n, int *parent, int a, int b, long wide, int *root) {
            volatile int pad[512];
            int local = 7;
            int *saved = &local;
            int result;
            pad[0] = n; pad[511] = n + 1;
            if (a != 1 || b != 2 || wide != 0x12345678L) return -1000;
            *parent = *parent + 1;
            *root = *root + 1;
            if (n == 60) ancestor = &local;
            if (n == 59 && *ancestor != 8) return -2000;
            if (n) result = walk(n-1, &local, a, b, wide, root) + 1;
            else result = 1;
            if (*saved != (n ? 8 : 7) || pad[0] != n || pad[511] != n+1) return -3000;
            return result;
        }
        int main(void) {
            rcc_stack_context context, other;
            int parent = 0; int root = 0; int result; unsigned peak;
            if (irq_abi_version() != 2) return 6;
            if (rcc_stack_reserve(&context, 32)) return 1;
            other = context;
            if (rcc_stack_destroy(&other) != -1) return 7;
            result = walk(120, &parent, 1, 2, 0x12345678L, &root);
            if (result != 121) return result;
            if (parent != 1) return 10000+parent;
            if (root != 121) return 11000+root;
            if (context.active_segments) return 12000+context.active_segments;
            peak = context.peak_segments;
            if (peak < 3) return 3;
            result = walk(120, &parent, 1, 2, 0x12345678L, &root);
            if (result != 121 || context.peak_segments != peak || context.active_segments) return 4;
            if (rcc_stack_destroy(&context)) return 5;
            return 42;
        }
    "#,
        42,
    );
}

#[test]
fn variadic_wide_values_callbacks_and_aggregate_returns_cross_segments() {
    run(
        r#"
        #include <stack.h>
        #include <stdarg.h>
        struct result { long long value; int *pointer; };
        long long wide_dive(int n) {
            volatile int pad[1024]; long long result;
            pad[0] = n;
            result = n ? wide_dive(n-1) : 0x123456789abcLL;
            if (pad[0] != n) return -1;
            return result;
        }
        struct result dive(int n, int *p) {
            volatile int pad[1024]; struct result r;
            pad[0] = n; pad[1023] = n + 1;
            if (n) r = dive(n-1, p);
            else { r.value = 0x123456789abcLL; r.pointer = p; }
            if (pad[0] != n || pad[1023] != n+1) r.value = -1;
            return r;
        }
        int check(int ignored, ...) {
            volatile int padding[7000];
            va_list args, copy;
            struct result (*callback)(int, int *) = dive;
            struct result r;
            int *p; long x; long long y;
            padding[0] = 11; padding[6999] = 22;
            va_start(args, ignored); va_copy(copy, args);
            p = va_arg(args, int *); x = va_arg(args, long); y = va_arg(args, long long);
            r = callback(40, p);
            if (va_arg(copy, int *) != p || va_arg(copy, long) != x || va_arg(copy, long long) != y) return 2;
            va_end(args); va_end(copy);
            if (r.pointer != p) return 23;
            if (*r.pointer != 42) return 24;
            if (r.value != y) return 25;
            if (x != 0x12345678L) return 26;
            if (padding[0] != 11 || padding[6999] != 22) return 27;
            return 42;
        }
        int main(void) {
            volatile int padding[1500];
            rcc_stack_context context; int n = 42; int result;
            padding[0] = 33; padding[1499] = 44;
            if (rcc_stack_reserve(&context, 16)) return 1;
            result = check(0, &n, 0x12345678L, 0x123456789abcLL);
            if (wide_dive(40) != 0x123456789abcLL) return 6;
            if (rcc_stack_destroy(&context)) return 4;
            if (padding[0] != 33 || padding[1499] != 44) return 5;
            return result;
        }
    "#,
        42,
    );
}

#[test]
fn exhausted_pool_stops_before_overwriting_another_bank() {
    run(
        r#"
        #include <stack.h>
        int dive(int n) { volatile int pad[1024]; pad[0]=n; return n ? dive(n-1)+pad[0] : 0; }
        int main(void) { rcc_stack_context context; if(rcc_stack_reserve(&context,1))return 1; return dive(100); }
    "#,
        125,
    );
}
