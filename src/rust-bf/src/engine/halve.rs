//! A portable BF binary-digit extraction idiom. This is a four-cell finite
//! state machine, independent of any guest CPU or ISA. Guarded native integer
//! arithmetic is just its exact closed form, like an ordinary affine loop.
use super::{Machine, Op, Program};
use std::sync::OnceLock;
const PATTERN: &str = "[->>+<[->>+<[-]<]>[-<+>]<<]";
#[derive(Debug)]
pub(super) struct Halve {
    stride: isize,
    next: usize,
}
impl Halve {
    pub(super) fn parse(p: &Program, at: usize) -> Option<Self> {
        let Op::Open(next) = p.code[at].op else {
            return None;
        };
        static TEMPLATE: OnceLock<Program> = OnceLock::new();
        let template = TEMPLATE.get_or_init(|| Program::parse(PATTERN, false, false).unwrap());
        if next - at != template.code.len() {
            return None;
        }
        let Op::Move(movement) = p.code.get(at + 2)?.op else {
            return None;
        };
        if movement == 0 || movement % 2 != 0 {
            return None;
        }
        let stride = movement / 2;
        for (actual, expected) in p.code[at..next].iter().zip(&template.code) {
            let matches = match (actual.op, expected.op) {
                (Op::Add(a), Op::Add(b)) => a == b,
                (Op::Move(a), Op::Move(b)) => Some(a) == b.checked_mul(stride),
                (Op::Open(a), Op::Open(b)) | (Op::Close(a), Op::Close(b)) => {
                    a.checked_sub(at) == Some(b)
                }
                _ => false,
            };
            if !matches {
                return None;
            }
        }
        Some(Self { stride, next })
    }
    pub(super) fn execute(&self, m: &mut Machine) -> bool {
        let n = m.tape[m.pointer];
        if n == 0 {
            m.pc = self.next;
            return true;
        }
        let Some(end) = self.stride.checked_mul(3) else {
            return false;
        };
        if !m.range_fits(end.min(0), end.max(0)) {
            return false;
        }
        let rem = (m.pointer as isize + self.stride) as usize;
        let flag = (m.pointer as isize + 2 * self.stride) as usize;
        let q = (m.pointer as isize + 3 * self.stride) as usize;
        if m.tape[rem] > 1 || m.tape[flag] != 0 {
            return false;
        }
        let total = u32::from(n) + u32::from(m.tape[rem]);
        m.write_tape(q, m.tape[q].wrapping_add((total / 2) as u16));
        m.write_tape(rem, (total % 2) as u16);
        m.write_tape(m.pointer, 0);
        m.pc = self.next;
        true
    }
}
#[cfg(test)]
mod tests {
    use super::*;
    use crate::engine::{tests::compare, Stop};
    #[test]
    fn closed_form_exhausts_all_16_bit_inputs_and_remainder_states() {
        let p = Program::parse(PATTERN, true, false).unwrap();
        assert!(Halve::parse(&p, 0).is_some());
        for n in 0..=65535u16 {
            for rem in [0u16, 1] {
                let mut m = Machine::new(4).unwrap();
                m.tape_mut().copy_from_slice(&[n, rem, 0, 65535]);
                assert_eq!(m.run(&p, 1).unwrap(), Stop::Halted);
                let total = u32::from(n) + u32::from(rem);
                assert_eq!(
                    m.tape(),
                    [
                        0,
                        (total % 2) as u16,
                        0,
                        65535u16.wrapping_add((total / 2) as u16)
                    ]
                );
                assert_eq!(m.pointer(), 0);
            }
        }
    }
    #[test]
    fn scaling_directions_guard_states_and_bounds_match_character_oracle() {
        for stride in [-7isize, -2, -1, 1, 2, 7] {
            let body: String = PATTERN
                .chars()
                .map(|c| match c {
                    '>' => if stride > 0 { ">" } else { "<" }.repeat(stride.unsigned_abs()),
                    '<' => if stride > 0 { "<" } else { ">" }.repeat(stride.unsigned_abs()),
                    _ => c.to_string(),
                })
                .collect();
            let start = if stride > 0 { 0 } else { 21 };
            let source = format!("{}{body}", ">".repeat(start));
            for n in [0, 1, 2, 3, 7, 31, 255] {
                for (rem, flag) in [(0, 0), (1, 0), (2, 0), (0, 3), (3, 7)] {
                    let mut initial = vec![0; 23];
                    initial[start] = n;
                    let r = (start as isize + stride) as usize;
                    let f = (start as isize + 2 * stride) as usize;
                    if r < initial.len() {
                        initial[r] = rem;
                    }
                    if f < initial.len() {
                        initial[f] = flag;
                    }
                    compare(&source, &initial, false, false);
                }
            }
            for cells in 1..=23 {
                let mut initial = vec![0; cells];
                initial[0] = 7;
                compare(&source, &initial, false, false);
            }
        }
        for barrier in ["!", "?", "$", ",", "."] {
            let altered = PATTERN.replacen(">>+", &format!(">{barrier}>+"), 1);
            let p = Program::parse(&altered, true, true).unwrap();
            assert!(Halve::parse(&p, 0).is_none());
        }
        // An odd shift/extra update isn't the certified finite-state machine.
        for source in [
            PATTERN.replacen(">>+", ">+", 1),
            PATTERN.replacen("+<", "++<", 1),
        ] {
            let p = Program::parse(&source, true, false).unwrap();
            assert!(Halve::parse(&p, 0).is_none());
        }
    }
    #[test]
    fn wrapping_aliases_retain_literal_nontermination_and_zero_skips() {
        for cells in 1..=4 {
            for optimize in [false, true] {
                let p = Program::parse(PATTERN, optimize, false).unwrap();
                let mut m = Machine::with_wrapping_tape(cells).unwrap();
                // A zero control must skip even on a tape too small for the body.
                assert_eq!(m.run(&p, 1).unwrap(), Stop::Halted);
            }
        }
        // Finite wrapping cases compare exact state and events.
        for cells in 2..=8 {
            let mut initial = vec![0; cells];
            initial[0] = 7;
            compare(PATTERN, &initial, true, false);
        }
    }
}
