//! Independent signed 16.16 oracles for the actual assembled Ripple leaves.
//! No engine/compiler implementation is used to compute the expected result.
use rvm::{vm::VMState, Instr, VM};
use std::{collections::BTreeMap, fs, path::PathBuf};

const BANK: usize = 64000;
const SEED: u32 = 0x73a9_f251;
const RANDOM_CASES: usize = 100_000;
const SAVED: [usize; 10] = [3, 4, 23, 24, 25, 26, 28, 29, 30, 31];

fn next(state: &mut u32) -> u32 {
    *state ^= *state << 13;
    *state ^= *state >> 17;
    *state ^= *state << 5;
    *state
}

fn divide(a: u32, b: u32) -> u32 {
    let a = a as i32 as i64;
    let b = b as i32 as i64;
    if (a.unsigned_abs() >> 14) >= b.unsigned_abs() {
        return if (a < 0) != (b < 0) { 0x80000000 } else { 0x7fffffff };
    }
    ((a * 65536) / b) as u32
}

fn validate(vm: &mut VM, name: &str, cases: &[(u32, u32)], state: &mut u32) -> String {
    let entry = *vm.debug_symbols.iter().find(|(_, symbol)| symbol.as_str() == name)
        .unwrap_or_else(|| panic!("missing {name} symbol")).0;
    let budget = if name == "FixedMul" { 100 } else { 700 };
    let mut histogram: BTreeMap<u64, usize> = BTreeMap::new();
    let mut classes = [0_usize; 4];
    let mut saturated = 0;
    for (index, &(a, b)) in cases.iter().enumerate() {
        for register in 1..32 { vm.registers[register] = next(state) as u16; }
        vm.registers[0] = 0;
        vm.registers[1] = (entry % BANK) as u16;
        vm.registers[2] = (entry / BANK) as u16;
        vm.registers[3] = 17;
        vm.registers[4] = 1;
        vm.registers[7] = a as u16;
        vm.registers[8] = (a >> 16) as u16;
        vm.registers[9] = b as u16;
        vm.registers[10] = (b >> 16) as u16;
        vm.registers[28] = 2;
        vm.registers[29] = 1000;
        vm.registers[30] = 900;
        vm.registers[31] = 1;
        let before_registers = vm.registers;
        vm.state = VMState::Running;
        let before = vm.executed_instructions;
        while matches!(vm.state, VMState::Running) {
            assert!(vm.executed_instructions - before <= budget,
                    "{name} instruction budget: case {index}, a={a:#010x}, b={b:#010x}");
            vm.step().unwrap();
        }
        assert!(matches!(vm.state, VMState::Halted), "kernel must return to caller trap");
        let steps = vm.executed_instructions - before - 1;
        assert!(steps <= budget, "{name} exceeded {budget}-instruction budget");
        assert_eq!(vm.registers[2], 1, "returned to the wrong code bank");
        assert_eq!(vm.registers[1], 18, "returned to the wrong code offset (HALT advances PC)");
        let actual = u32::from(vm.registers[5]) | (u32::from(vm.registers[6]) << 16);
        let expected = if name == "FixedMul" {
            (((a as i32 as i64) * (b as i32 as i64)) >> 16) as u32
        } else { divide(a, b) };
        assert_eq!(actual, expected, "case {index}: {name}({a:#010x}, {b:#010x})");
        for register in SAVED {
            assert_eq!(vm.registers[register], before_registers[register],
                       "{name} case {index}: callee-saved register {register}");
        }
        assert_eq!(vm.registers[0], 0);
        *histogram.entry(steps).or_default() += 1;
        classes[usize::from(a as u16 == 0) + 2 * usize::from(b as u16 == 0)] += 1;
        if name == "FixedDiv" && ((a as i32 as i64).unsigned_abs() >> 14) >= (b as i32 as i64).unsigned_abs() {
            saturated += 1;
        }
    }
    assert!(vm.memory.iter().all(|word| *word == 0xface), "{name} modified guest memory");
    let min = histogram.keys().next().unwrap();
    let max = histogram.keys().next_back().unwrap();
    let bins = histogram.iter().map(|(steps, count)| format!("\"{steps}\":{count}"))
        .collect::<Vec<_>>().join(",");
    println!("{name}: {} cases passed; {min}..{max} guest instructions, exact signed 64-bit oracle, saved registers, cross-bank return and memory guards", cases.len());
    format!("{{\"name\":\"{name}\",\"cases\":{},\"min_instructions\":{min},\"max_instructions\":{max},\"instruction_histogram\":{{{bins}}},\"saturation_cases\":{saturated},\"path_cases\":{{\"fractional\":{},\"a_integer\":{},\"b_integer\":{},\"both_integer\":{}}}}}",
            cases.len(), classes[0], classes[1], classes[2], classes[3])
}

fn main() {
    let out = PathBuf::from(std::env::args().nth(1).expect("output directory"));
    let mut vm = VM::with_options(BANK as u16, BANK * 4, Some(out.join("test.disk")));
    vm.host_input_enabled = false;
    vm.load_binary(&fs::read(out.join("fixed_test.bin")).unwrap()).unwrap();
    // A return trap in a second code bank checks RAB as well as RA. Padding
    // changes only the test caller; every kernel instruction comes from rasm.
    vm.instructions.resize(BANK + 32, Instr::new(0, 0, 0, 0, 0));
    vm.memory.fill(0xface);
    let edge: [u32; 31] = [
        0, 1, 2, 0x7f, 0x80, 0xff, 0x100, 0x101, 0x7fff, 0x8000,
        0xff00, 0xffff, 0x10000, 0x10001, 0x17fff, 0x18000, 0x1ffff,
        0x12345678, 0x7fff0000, 0x7ffffffe, 0x7fffffff, 0x80000000,
        0x80000001, 0x80008000, 0x89abcdef, 0xfffeffff, 0xffff0000,
        0xffff0001, 0xffff8000, 0xfffffffe, 0xffffffff,
    ];
    let mut cases = Vec::new();
    for a in edge { for b in edge { cases.push((a, b)); } }
    // Explicitly combine every bit position with every other bit position,
    // including sign bits and carries at each 8/16-bit product boundary.
    for a in 0..32 { for b in 0..32 {
        cases.push((1_u32 << a, 1_u32 << b));
        cases.push(((1_u32 << a).wrapping_neg(), (1_u32 << b).wrapping_neg()));
    } }
    let mut state = SEED;
    for _ in 0..RANDOM_CASES { cases.push((next(&mut state), next(&mut state))); }
    // The general random distribution rarely exercises exact integer fast
    // paths. Force both one-integer variants and the two-integer variant too.
    for _ in 0..10_000 {
        let a = next(&mut state);
        let b = next(&mut state);
        cases.extend([(a & 0xffff0000, b), (a, b & 0xffff0000),
                      (a & 0xffff0000, b & 0xffff0000)]);
    }
    let multiply = validate(&mut vm, "FixedMul", &cases, &mut state);
    // Probe both sides of Doom's conservative guard with each denominator sign.
    for _ in 0..2000 {
        let a = next(&mut state);
        let threshold = (a as i32 as i64).unsigned_abs() as u32 >> 14;
        for b in [threshold.saturating_sub(1), threshold, threshold + 1, threshold + 2] {
            cases.extend([(a, b), (a, b.wrapping_neg())]);
        }
    }
    let division = validate(&mut vm, "FixedDiv", &cases, &mut state);
    let report = format!("{{\"success\":true,\"random_full_width_cases_per_operation\":{RANDOM_CASES},\"seed\":{SEED},\"operations\":[{multiply},{division}],\"callee_saved_and_return_checked\":true,\"guest_memory_unchanged\":true}}\n");
    fs::write(out.join("results.json"), report).unwrap();
}
