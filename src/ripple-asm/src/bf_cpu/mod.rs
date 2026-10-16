//! Additional CPU microcode generated as portable 16-bit Brainfuck. Static
//! addresses use the otherwise unused register lane, beyond the 32 registers.
//! Keeping this emitter separate avoids another large, opaque macro routine.
mod arithmetic;
mod code;
mod emitter;
mod interrupts;
mod memory;
use emitter::{cell, Bf};
pub(super) const MIN_WORDS: usize = 224;
pub const ERROR_WORD: usize = 57;
pub const MAGIC_WORD: usize = 80;
pub const EXTENT_BANK_WORD: usize = 81;
pub const EXTENT_OFF_WORD: usize = 82;
pub const EXTENT_FULL_WORD: usize = 87;
// Published by new CPUs for host bulk code uploads. Older images leave these
// zero and continue to use STORC through the guest fallback.
pub const CAPACITY_BANK_WORD: usize = 93;
pub const CAPACITY_OFF_WORD: usize = 94;
pub const CAPACITY_FULL_WORD: usize = 95;
pub const MAGIC: u16 = 0xbfc2;
const W1: usize = cell(58);
const W2: usize = cell(59);
const W3: usize = cell(60);
const VALUE: usize = cell(61);
const RESULT: usize = cell(63);
const VALID: usize = cell(64);
const BANK: usize = cell(65);
const OFF: usize = cell(66);
const CMP: usize = cell(67);
const TMP: usize = cell(68);
const JUMP: usize = cell(56);
const ERROR: usize = cell(ERROR_WORD);
const PENDING: usize = cell(45);
const ENABLE: usize = cell(46);
const BUSY: usize = cell(47);
const CAUSE: usize = cell(50);
const ACK: usize = cell(51);
const REM_LO: usize = cell(52);
const REM_HI: usize = cell(53);
const TIMER: usize = cell(54);
const RESET: usize = cell(55);
const fn reg(r: usize) -> usize {
    cell(r + 1)
}
const fn csr(c: usize) -> usize {
    cell(33 + c)
}
const fn pending(bit: usize) -> usize {
    cell(96 + bit)
}
const fn enabled(bit: usize) -> usize {
    cell(112 + bit)
}
fn fault(b: &mut Bf, error: u16) {
    b.set(ERROR, error);
    b.set(0, 1);
}
fn read_register(b: &mut Bf, selector: usize, dest: usize) {
    b.clear(VALID);
    for r in 0..32 {
        b.eq(selector, r as u16, |b| {
            b.copy(reg(r), dest);
            b.set(VALID, 1);
        });
    }
    b.zero(VALID, |b| fault(b, 1));
}
fn write_register(b: &mut Bf, selector: usize, value: usize) {
    b.clear(VALID);
    for r in 0..32 {
        b.eq(selector, r as u16, |b| {
            if r != 0 {
                b.copy(value, reg(r));
            }
            b.set(VALID, 1);
        });
    }
    b.zero(VALID, |b| fault(b, 1));
}
// Decode only when a mask is written, never on each instruction boundary.
fn decode(b: &mut Bf, source: usize, base: usize, or: bool, clear: bool) {
    let n = cell(69);
    let q = cell(70);
    let rem = cell(71);
    let flag = cell(72);
    b.copy(source, n);
    for bit in 0..16 {
        b.clear(q);
        b.clear(rem);
        b.loop_(n, |b| {
            b.add(n, u16::MAX);
            b.set(flag, 1);
            b.if_(rem, |b| {
                b.clear(rem);
                b.add(q, 1);
                b.clear(flag);
            });
            b.if_(flag, |b| b.set(rem, 1));
        });
        if clear {
            b.if_(rem, |b| b.clear(cell(base + bit)));
        } else if or {
            b.if_(rem, |b| b.set(cell(base + bit), 1));
        } else {
            b.copy(rem, cell(base + bit));
        }
        b.take(q, n);
    }
    for p in [n, q, rem, flag] {
        b.clear(p);
    }
}
fn encode_pending(b: &mut Bf) {
    b.clear(PENDING);
    for bit in 0..16 {
        b.if_(pending(bit), |b| b.add(PENDING, 1u16 << bit));
    }
}
fn sync_mmio(b: &mut Bf) {
    for (word, src) in [
        (21, PENDING),
        (22, ENABLE),
        (23, BUSY),
        (24, cell(48)),
        (25, cell(49)),
        (26, CAUSE),
    ] {
        b.copy(src, word * 8 + 4);
    }
}
/// Capacity is bounded by the already declared physical tape, leaving the ALU
/// scratch and guard untouched. Growing code never changes guest RAM capacity.
pub(super) fn capacity(instructions: usize, bank: u16, banks: Option<u16>) -> usize {
    let words = (banks.map_or(0, |n| usize::from(n) * usize::from(bank)))
        .max(76 + 4 * instructions)
        .max(MIN_WORDS);
    ((words - 76) / 4 + 1)
        .min(64 * 65536)
        .min(usize::from(bank) * 65536)
}
pub(super) fn definitions(
    bank: u16,
    banks: Option<u16>,
    instructions: usize,
    code_banks: u16,
) -> String {
    let cap = capacity(instructions, bank, banks.or(Some(16)))
        .max(usize::from(code_banks) * usize::from(bank));
    let mut definitions = String::new();
    let mut emit = |name: &str, b: Bf| {
        definitions.push_str(&format!("#define {name} {{preserve({})}}\n", b.finish()));
    };
    for (name, op) in arithmetic::operations() {
        emit(name, arithmetic::generate(op));
    }
    let mut boot = Bf::default();
    boot.set(cell(MAGIC_WORD), MAGIC);
    boot.set(
        cell(EXTENT_BANK_WORD),
        (instructions / usize::from(bank)) as u16,
    );
    boot.set(
        cell(EXTENT_OFF_WORD),
        (instructions % usize::from(bank)) as u16,
    );
    boot.set(cell(CAPACITY_BANK_WORD), (cap / usize::from(bank)) as u16);
    boot.set(cell(CAPACITY_OFF_WORD), (cap % usize::from(bank)) as u16);
    boot.set(
        cell(CAPACITY_FULL_WORD),
        u16::from(cap / usize::from(bank) == 65536),
    );
    emit("cpu_boot", boot);
    let mut disabled = Bf::default();
    fault(&mut disabled, 8);
    emit("cpu_irq_disabled", disabled);
    emit("cpu_irq_boundary", interrupts::boundary(bank));
    emit("cpu_timer_tick", interrupts::tick());
    emit("cpu_csrr", interrupts::read_csr());
    emit("cpu_csrw", interrupts::write_csr(bank, banks.or(Some(16))));
    emit("cpu_iret", interrupts::iret());
    emit("cpu_loadc", code::access(bank, cap, false));
    emit("cpu_storc", code::access(bank, cap, true));
    emit("cpu_irq_store", interrupts::store());
    emit("cpu_ram_load", memory::access(bank, banks.is_some(), false));
    emit("cpu_ram_store", memory::access(bank, banks.is_some(), true));
    definitions
}
