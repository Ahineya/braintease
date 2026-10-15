//! The JIT must match the interpreter at every batch boundary, including
//! self-modifying STORC and bank-zero MMIO yields.
use rvm::{vm::VMState, Instr, VM};
use std::{
    fs,
    path::PathBuf,
    sync::atomic::{AtomicU64, Ordering},
};

static ID: AtomicU64 = AtomicU64::new(0);

struct Fixture {
    vm: VM,
    path: PathBuf,
}

impl Fixture {
    fn new(bank: u16, code: Vec<Instr>) -> Self {
        let path = std::env::temp_dir().join(format!(
            "rvm-jit-{}-{}.img",
            std::process::id(),
            ID.fetch_add(1, Ordering::Relaxed)
        ));
        let mut vm = VM::with_options(bank, 8192, Some(path.clone()));
        vm.state = VMState::Running;
        vm.host_input_enabled = false;
        vm.instructions = code;
        Self { vm, path }
    }
}

impl Drop for Fixture {
    fn drop(&mut self) {
        let _ = fs::remove_file(&self.path);
    }
}

fn i(op: u8, a: u16, b: u16, c: u16) -> Instr {
    Instr::new(op, op, a, b, c)
}

fn same(a: &VM, b: &VM) {
    assert_eq!(
        a.registers, b.registers,
        "registers jit_n={} ref_n={}",
        a.executed_instructions, b.executed_instructions
    );
    assert_eq!(a.memory, b.memory, "memory");
    let code = |vm: &VM| {
        vm.instructions
            .iter()
            .map(|inst| (inst.opcode, inst.word0, inst.word1, inst.word2, inst.word3))
            .collect::<Vec<_>>()
    };
    assert_eq!(code(a), code(b), "instructions");
    assert_eq!(format!("{:?}", a.state), format!("{:?}", b.state), "state");
    assert_eq!(a.executed_instructions, b.executed_instructions, "count");
    assert_eq!(a.output_buffer, b.output_buffer, "output");
    assert_eq!(a.device_error, b.device_error, "device error");
}

fn differential(bank: u16, code: Vec<Instr>, budget: u64) {
    let mut jit = Fixture::new(bank, code.clone());
    let mut reference = Fixture::new(bank, code);
    jit.vm.jit_enabled = true;
    for _ in 0..8000 {
        let jit_result = jit.vm.run_batch(budget);
        let reference_result = reference.vm.run_batch(budget);
        match (jit_result, reference_result) {
            (Ok(left), Ok(right)) => assert_eq!(left, right, "batch length"),
            (Err(left), Err(right)) => {
                assert_eq!(left, right, "error");
                same(&jit.vm, &reference.vm);
                return;
            }
            (left, right) => panic!("status diverged: jit={left:?} interpreter={right:?}"),
        }
        same(&jit.vm, &reference.vm);
        if !matches!(jit.vm.state, VMState::Running) {
            return;
        }
    }
    panic!("fixture did not terminate");
}

fn check(bank: u16, code: Vec<Instr>) {
    for budget in [1, 7, 64, 10_000] {
        differential(bank, code.clone(), budget);
    }
}

const T0: u16 = 15;
const T1: u16 = 16;
const T2: u16 = 17;
const T3: u16 = 18;
const A0: u16 = 7;
const RA: u16 = 3;
const X0: u16 = 11;
const X1: u16 = 12;
const X2: u16 = 13;
const X3: u16 = 14;

#[test]
fn arithmetic_constant_chain_matches() {
    // LI T0, 42; ADDI T1, T0, 10; MUL T2, T1, T1; HALT
    check(
        64,
        vec![
            i(0x0E, T0, 42, 0),
            i(0x0A, T1, T0, 10),
            i(0x1A, T2, T1, T1),
            i(0x00, 0, 0, 0),
        ],
    );
}

#[test]
fn compares_shifts_and_div0_match() {
    check(
        64,
        vec![
            i(0x0E, T0, 0xFFFF, 0),
            i(0x0E, T1, 1, 0),
            i(0x08, T2, T0, T1),
            i(0x09, T3, T0, T1),
            i(0x06, T2, T1, T1), // SLL by the register holding 1, after T2 was the flag
            i(0x1B, T0, T0, 0),  // DIV by R0
            i(0x1C, T1, T1, 0),
            i(0x0E, T0, 5, 0),
            i(0x00, 0, 0, 0),
        ],
    );
}

#[test]
fn loop_reenters_a_compiled_block() {
    // LI T0, 1000; loop: ADDI T0, T0, -1; BNE T0, R0, -1; HALT
    check(
        64,
        vec![
            i(0x0E, T0, 1000, 0),
            i(0x0A, T0, T0, 0xFFFF),
            i(0x16, T0, 0, 0xFFFF),
            i(0x00, 0, 0, 0),
        ],
    );
}

#[test]
fn load_store_and_mmio_yield_match() {
    check(
        64,
        vec![
            i(0x0E, T0, 40, 0),
            i(0x0E, T1, 1, 0),
            i(0x0E, A0, 123, 0),
            i(0x12, A0, T1, T0),
            i(0x11, T2, T1, T0),
            i(0x0E, T0, 5, 0),
            i(0x0E, A0, 99, 0),
            i(0x12, A0, 0, T0), // bank 0, addr 5: header store yields
            i(0x0E, T1, 1, 0),
            i(0x00, 0, 0, 0),
        ],
    );
}

#[test]
fn jumps_and_bank_wrap_match() {
    // Bank size 4 wraps after the LI at index 3. JALR then lands on the HALT.
    check(
        4,
        vec![
            i(0x13, RA, 0, 3),
            i(0x42, 0, 0, 0),
            i(0x00, 0, 0, 0),
            i(0x0E, T0, 4, 0),
            i(0x0E, T1, 2, 0),
            i(0x0E, T2, 0, 0),
            i(0x14, RA, T2, T1),
            i(0x0E, T3, 8, 0),
        ],
    );
}

#[test]
fn r0_and_invalid_registers_stay_zero() {
    check(
        64,
        vec![
            i(0x0E, 0, 5, 0),
            i(0x01, T0, 0, 0),
            i(0x01, 40, T0, T0),
            i(0x00, 0, 0, 0),
        ],
    );
}

#[test]
fn storc_invalidates_the_block_that_covered_the_write() {
    // Jump to the first LI T0, 1. When T0 is still 1, patch that LI into
    // LI T0, 7 and run it again. A stale block would leave T0 at 1 and loop.
    check(
        32,
        vec![
            i(0x13, 0, 0, 10),
            i(0x0E, X0, 0x0E, 0),
            i(0x0E, X1, T0, 0),
            i(0x0E, X2, 7, 0),
            i(0x0E, X3, 0, 0),
            i(0x0E, T1, 10, 0),
            i(0x0E, T2, 0, 0),
            i(0x21, 0, T2, T1),
            i(0x13, 0, 0, 10),
            i(0x00, 0, 0, 0),
            i(0x0E, T0, 1, 0),
            i(0x0E, T3, 1, 0),
            i(0x16, T0, T3, 0xFFFD), // PC 12 -> 9 when T0 != 1
            i(0x13, 0, 0, 1),
        ],
    );
}

#[test]
fn unknown_opcode_and_bad_load_match() {
    check(64, vec![Instr::new(0x7F, 0x7F, 0, 0, 0)]);
    check(
        64,
        vec![
            i(0x0E, T0, 0, 0),
            i(0x0E, T1, 128, 0),
            i(0x11, T2, T1, T0),
            i(0x00, 0, 0, 0),
        ],
    );
    check(64, vec![i(0x13, 0, 0, 50), i(0x00, 0, 0, 0)]);
}

/// Release timing for a long ALU loop. Not part of the default suite.
#[test]
#[ignore]
fn tight_loop_timing() {
    // 200 * 65535 decrements. Offset from the outer BNE (PC 5) back to PC 1 is -4.
    let code = vec![
        i(0x0E, T1, 200, 0),
        i(0x0E, T0, 65535, 0),
        i(0x0A, T0, T0, 0xFFFF),
        i(0x16, T0, 0, 0xFFFF),
        i(0x0A, T1, T1, 0xFFFF),
        i(0x16, T1, 0, 0xFFFC),
        i(0x00, 0, 0, 0),
    ];
    let time = |jit: bool| {
        let mut fx = Fixture::new(64, code.clone());
        fx.vm.jit_enabled = jit;
        let started = std::time::Instant::now();
        loop {
            fx.vm.run_batch(4096).unwrap();
            if !matches!(fx.vm.state, VMState::Running) {
                break;
            }
        }
        (
            started.elapsed(),
            fx.vm.executed_instructions,
            fx.vm.registers[T0 as usize],
            fx.vm.registers[T1 as usize],
        )
    };
    let interp = time(false);
    let jit = time(true);
    assert_eq!(interp.1, jit.1);
    assert_eq!(interp.2, 0);
    assert_eq!(jit.2, 0);
    assert_eq!(interp.3, 0);
    assert_eq!(jit.3, 0);
    eprintln!("interp {:?}  jit {:?}", interp.0, jit.0);
}

#[test]
fn irq_before_a_block_matches() {
    use rvm::constants::{HDR_IRQ_ENABLE, HDR_IRQ_STATUS, HDR_IRQ_VECTOR_OFF};
    // Arm the controller with guest stores. The instruction after the status
    // write is a NOP; a taken IRQ skips it and runs LI RV0, 99.
    check(
        64,
        vec![
            i(0x0E, T0, HDR_IRQ_VECTOR_OFF as u16, 0),
            i(0x0E, A0, 12, 0),
            i(0x12, A0, 0, T0),
            i(0x0E, T0, HDR_IRQ_ENABLE as u16, 0),
            i(0x0E, A0, 1, 0),
            i(0x12, A0, 0, T0),
            i(0x0E, T0, HDR_IRQ_STATUS as u16, 0),
            i(0x0E, A0, 1, 0),
            i(0x12, A0, 0, T0),
            i(0x42, 0, 0, 0),
            i(0x00, 0, 0, 0),
            i(0x42, 0, 0, 0),
            i(0x0E, 5, 99, 0),
            i(0x00, 0, 0, 0),
        ],
    );
}
