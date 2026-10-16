//! Independent bounded BF evaluator: no Ripple decoding or CPU shortcuts.
use super::{generate, operations, Operation, LHS, OUT, RHS};
#[derive(Clone)]
enum Op {
    Add(u16),
    Move(isize),
    Clear,
    Open(usize),
    Close(usize),
    Affine(Vec<(isize, u16)>),
}
fn parse(code: &str, affine: bool) -> Vec<Op> {
    let bytes = code.as_bytes();
    let mut ops = Vec::new();
    let mut stack = Vec::new();
    let mut i = 0;
    while i < bytes.len() {
        let c = bytes[i];
        if bytes[i..].starts_with(b"[-]") {
            ops.push(Op::Clear);
            i += 3;
            continue;
        }
        if b"+-<>".contains(&c) {
            let movement = b"<>".contains(&c);
            let mut n = 0isize;
            while i < bytes.len()
                && (if movement {
                    b"<>".contains(&bytes[i])
                } else {
                    b"+-".contains(&bytes[i])
                })
            {
                n += if b">+".contains(&bytes[i]) { 1 } else { -1 };
                i += 1;
            }
            ops.push(if movement {
                Op::Move(n)
            } else {
                Op::Add(n as u16)
            });
            continue;
        }
        match c {
            b'[' => {
                stack.push(ops.len());
                ops.push(Op::Open(0));
            }
            b']' => {
                let start = stack.pop().unwrap();
                let mut changes = Vec::new();
                let mut offset = 0;
                let simple = ops[start + 1..].iter().all(|op| match op {
                    Op::Move(n) => {
                        offset += n;
                        true
                    }
                    Op::Add(n) => {
                        changes.push((offset, *n));
                        true
                    }
                    _ => false,
                });
                let delta = changes
                    .iter()
                    .filter(|(p, _)| *p == 0)
                    .fold(0u16, |a, (_, b)| a.wrapping_add(*b));
                if affine && simple && offset == 0 && delta == 65535 {
                    ops.truncate(start);
                    ops.push(Op::Affine(changes));
                } else {
                    let end = ops.len();
                    ops[start] = Op::Open(end);
                    ops.push(Op::Close(start));
                }
            }
            _ => panic!("microcode must contain only BF: {c}"),
        }
        i += 1;
    }
    assert!(stack.is_empty());
    ops
}
fn check(ops: &[Op], op: Operation, lhs: u16, rhs: u16) -> u64 {
    let mut tape = vec![0u16; 256];
    // All lanes the ALU doesn't own are deliberately nonzero, including
    // instruction data, RAM, and persistent CPU state in the register lane.
    for (word, cells) in tape.chunks_exact_mut(8).enumerate() {
        for lane in [1, 3, 4] {
            cells[lane] = (word as u16)
                .wrapping_mul(193)
                .wrapping_add(lane as u16 + 32768);
        }
    }
    let mut expected = tape.clone();
    tape[LHS] = lhs;
    tape[RHS] = rhs;
    let result = match op {
        Operation::And => lhs & rhs,
        Operation::Or => lhs | rhs,
        Operation::Xor => lhs ^ rhs,
        Operation::Left => lhs.wrapping_shl(u32::from(rhs & 15)),
        Operation::Right => lhs >> (rhs & 15),
        Operation::Div => lhs.checked_div(rhs).unwrap_or(0),
        Operation::Mod => lhs.checked_rem(rhs).unwrap_or(0),
    };
    expected[OUT] = result;
    let (mut pc, mut p, mut steps) = (0, 0usize, 0u64);
    while pc < ops.len() {
        steps += 1;
        assert!(steps < 20_000_000, "nontermination: {lhs}, {rhs}, pc={pc}");
        match &ops[pc] {
            Op::Add(n) => tape[p] = tape[p].wrapping_add(*n),
            Op::Move(n) => p = usize::try_from(p as isize + n).unwrap(),
            Op::Clear => tape[p] = 0,
            Op::Open(end) if tape[p] == 0 => pc = *end,
            Op::Close(start) if tape[p] != 0 => pc = *start,
            Op::Affine(changes) => {
                let n = tape[p];
                for (d, v) in changes {
                    let q = usize::try_from(p as isize + d).unwrap();
                    tape[q] = tape[q].wrapping_add(n.wrapping_mul(*v));
                }
            }
            _ => {}
        }
        pc += 1;
    }
    assert_eq!(p, 0);
    assert_eq!(tape, expected, "operands={lhs},{rhs}");
    steps
}
#[test]
fn local_arithmetic_edges_and_samples_preserve_every_other_lane() {
    let edges = [
        0, 1, 2, 3, 15, 16, 17, 255, 256, 32767, 32768, 32769, 65534, 65535,
    ];
    for (_, operation) in operations() {
        let code = generate(operation).finish();
        let ops = parse(&code, true);
        for lhs in edges {
            for rhs in edges {
                check(&ops, operation, lhs, rhs);
            }
        }
        let mut seed = 0x516be7u32;
        for _ in 0..128 {
            seed = seed.wrapping_mul(1664525).wrapping_add(1013904223);
            let lhs = seed as u16;
            seed = seed.wrapping_mul(1664525).wrapping_add(1013904223);
            check(&ops, operation, lhs, seed as u16);
        }
    }
}
#[test]
fn local_arithmetic_also_runs_as_literal_bf() {
    for (_, operation) in operations() {
        let ops = parse(&generate(operation).finish(), false);
        for (lhs, rhs) in [(0, 0), (12, 3), (65535, 65535), (32768, 17)] {
            check(&ops, operation, lhs, rhs);
        }
    }
}
#[test]
fn long_division_exhausts_small_operands_and_high_divisor_borrow() {
    for operation in [Operation::Div, Operation::Mod] {
        let ops = parse(&generate(operation).finish(), true);
        for lhs in 0..64 {
            for rhs in 0..64 {
                check(&ops, operation, lhs, rhs);
            }
        }
        for rhs in [32768u16, 32769, 49151, 65534, 65535] {
            for lhs in [rhs - 1, rhs, rhs.saturating_add(1), 65535] {
                check(&ops, operation, lhs, rhs);
            }
        }
    }
}
