//! Local ALU microcode. Inputs are scratch-A at operand words 1 (lhs) and
//! 0 (rhs); output is scratch-B at opcode word 3. Only START/PTR/SA/SB and the
//! unused fast lane *after* the opcode are scratch. REG/CODE/RAM stay intact.
//! No value-sized loop contains a tape scan, and no host arithmetic is used.
use super::emitter::Bf;

#[derive(Clone, Copy)]
pub(super) enum Operation {
    And,
    Or,
    Xor,
    Left,
    Right,
    Div,
    Mod,
}
pub(super) fn operations() -> [(&'static str, Operation); 7] {
    use Operation::*;
    [
        ("cpu_alu_and", And),
        ("cpu_alu_or", Or),
        ("cpu_alu_xor", Xor),
        ("cpu_alu_sl", Left),
        ("cpu_alu_sr", Right),
        ("cpu_alu_div", Div),
        ("cpu_alu_mod", Mod),
    ]
}
const LHS: usize = 13;
const RHS: usize = 5;
const OUT: usize = 30;
const N: usize = 37;
const REM: usize = 38;
const FLAG: usize = 39;
const Q: usize = 40;
const SUM: usize = 32;
const BORROW: usize = 34;
const COUNT: usize = 45;
const TEMP: usize = 46;
const COPY: usize = 42;
const fn a(i: usize) -> usize {
    (6 + i) * 8 + 5
}
const fn d(i: usize) -> usize {
    (6 + i) * 8 + 6
}
const fn r(i: usize) -> usize {
    (6 + i) * 8
}
const fn difference(i: usize) -> usize {
    (6 + i) * 8 + 2
}

/// Consume N into Q=floor(N/2), REM=N%2. Toggle a one-bit remainder with a
/// one-shot flag; all inner counters are 0/1. The head stays within four cells
/// and returns to N. This replaces general division by two at an eight-cell
/// stride for every extracted bit.
fn halve(b: &mut Bf) {
    b.at(N);
    b.raw("[->>+<[->>+<[-]<]>[-<+>]<<]");
}
fn unpack(b: &mut Bf, input: usize, bit: fn(usize) -> usize) {
    b.take(input, N);
    for i in 0..16 {
        halve(b);
        b.take(REM, bit(i));
        b.take(Q, N);
    }
}
/// Horner reconstruction: result = result*2 + next_bit. All value-sized
/// transfers are local affine loops, rather than one distant scan per unit.
fn pack(b: &mut Bf, bit: fn(usize) -> usize) {
    for i in (0..16).rev() {
        b.loop_(OUT, |b| {
            b.add(OUT, u16::MAX);
            b.add(TEMP, 2);
        });
        b.take(TEMP, OUT);
        b.take(bit(i), OUT);
    }
}
fn shifts(b: &mut Bf, left: bool) {
    b.take(RHS, N);
    // ISA shift counts use their low four bits, including register counts.
    for i in 0..4 {
        halve(b);
        b.loop_(REM, |b| {
            b.add(REM, u16::MAX);
            b.add(COUNT, 1 << i);
        });
        b.take(Q, N);
    }
    b.clear(N);
    b.take(LHS, N);
    b.loop_(COUNT, |b| {
        b.add(COUNT, u16::MAX);
        if left {
            b.loop_(N, |b| {
                b.add(N, u16::MAX);
                b.add(Q, 2);
            });
        } else {
            halve(b);
            b.clear(REM);
        }
        b.take(Q, N);
    });
    b.take(N, OUT);
}
fn division(b: &mut Bf, modulo: bool) {
    // Detect zero once; DIV and MOD both return zero for a zero divisor.
    for i in 0..16 {
        b.if_(d(i), |b| b.set(COUNT, 1));
    }
    b.loop_(COUNT, |b| {
        b.clear(COUNT);
        // Restoring long division. Keep the 17th remainder bit explicitly;
        // truncating it would break divisors >= 32768.
        for bit in (0..16).rev() {
            b.take(r(15), FLAG);
            for i in (1..16).rev() {
                b.take(r(i - 1), r(i));
            }
            b.take(a(bit), r(0));
            for i in 0..16 {
                b.take(BORROW, SUM);
                b.copy(d(i), TEMP);
                b.take(TEMP, SUM);
                b.copy(r(i), difference(i));
                b.eq(SUM, 1, |b| {
                    b.set(TEMP, 1);
                    b.loop_(difference(i), |b| {
                        b.clear(difference(i));
                        b.clear(TEMP);
                    });
                    b.copy(TEMP, difference(i));
                    b.take(TEMP, BORROW);
                });
                b.eq(SUM, 2, |b| b.set(BORROW, 1));
                b.clear(SUM);
            }
            b.loop_(FLAG, |b| {
                b.clear(FLAG);
                b.clear(BORROW);
            });
            b.zero(BORROW, |b| {
                for i in 0..16 {
                    b.clear(r(i));
                    b.take(difference(i), r(i));
                }
                b.set(a(bit), 1);
            });
            b.clear(BORROW);
            for i in 0..16 {
                b.clear(difference(i));
            }
        }
        pack(b, if modulo { r } else { a });
    });
    for i in 0..16 {
        b.clear(a(i));
        b.clear(d(i));
        b.clear(r(i));
    }
}
pub(super) fn generate(op: Operation) -> Bf {
    use Operation::*;
    let mut b = Bf::local(COPY, 24 * 8 + 5);
    if matches!(op, Left | Right) {
        shifts(&mut b, matches!(op, Left));
    } else {
        unpack(&mut b, LHS, a);
        unpack(&mut b, RHS, d);
        if matches!(op, Div | Mod) {
            division(&mut b, matches!(op, Mod));
        } else {
            for i in 0..16 {
                match op {
                    And => {
                        b.loop_(a(i), |b| {
                            b.clear(a(i));
                            b.take(d(i), r(i));
                        });
                        b.clear(d(i));
                    }
                    Or => {
                        for p in [a(i), d(i)] {
                            b.loop_(p, |b| {
                                b.clear(p);
                                b.set(r(i), 1);
                            });
                        }
                    }
                    Xor => {
                        b.take(a(i), r(i));
                        b.take(d(i), r(i));
                        b.eq(r(i), 2, |b| b.clear(r(i)));
                    }
                    _ => unreachable!(),
                }
            }
            pack(&mut b, r);
        }
    }
    b
}

#[cfg(test)]
#[path = "arithmetic_tests.rs"]
mod tests;
