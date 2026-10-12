//! A generic moving-marker search for the next nonzero cell.
//!
//! The loop `[>[<-]<[->+<]>]` moves a one through zero cells to its right.
//! Mirroring each move searches left; repeating each move searches a fixed stride.
//! Its fast path requires that marker to be one and the cell behind it zero.
//! No CPU layout or guest instruction information is involved.

use super::{Machine, Op, Program, SCAN_LIMIT};

#[derive(Debug)]
pub(super) struct Search {
    stride: isize,
    minimum: isize,
    maximum: isize,
    next: usize,
}

impl Search {
    pub(super) fn parse(program: &Program, at: usize) -> Option<Self> {
        let code = program.code.get(at..at.checked_add(15)?)?;
        let Op::Open(next) = code[0].op else {
            return None;
        };
        let Op::Move(stride) = code[1].op else {
            return None;
        };
        if stride == 0 || next != at + 15 {
            return None;
        }
        let reverse = stride.checked_neg()?;
        if !matches!(code[2].op, Op::Open(n) if n == at + 6)
            || !matches!(code[3].op, Op::Move(n) if n == reverse)
            || !matches!(code[4].op, Op::Add(u16::MAX))
            || !matches!(code[5].op, Op::Close(n) if n == at + 3)
            || !matches!(code[6].op, Op::Move(n) if n == reverse)
            || !matches!(code[7].op, Op::Open(n) if n == at + 13)
            || !matches!(code[8].op, Op::Add(u16::MAX))
            || !matches!(code[9].op, Op::Move(n) if n == stride)
            || !matches!(code[10].op, Op::Add(1))
            || !matches!(code[11].op, Op::Move(n) if n == reverse)
            || !matches!(code[12].op, Op::Close(n) if n == at + 8)
            || !matches!(code[13].op, Op::Move(n) if n == stride)
            || !matches!(code[14].op, Op::Close(n) if n == at + 1)
        {
            return None;
        }
        Some(Self {
            stride,
            minimum: stride.min(reverse),
            maximum: stride.max(reverse),
            next,
        })
    }

    pub(super) fn execute(&self, machine: &mut Machine) -> bool {
        // The found-cell path also visits the cell behind the marker. Require
        // the full excursion to fit, so wrapping aliases and boundary errors
        // use the original instructions with their exact intermediate state.
        if machine.tape[machine.pointer] != 1 || !machine.range_fits(self.minimum, self.maximum) {
            return false;
        }
        let behind = (machine.pointer as isize - self.stride) as usize;
        if machine.tape[behind] != 0 {
            return false;
        }
        let original = machine.pointer;
        for _ in 0..SCAN_LIMIT {
            if !machine.range_fits(self.minimum, self.maximum) {
                // Previous iterations are complete BF loop iterations, with
                // the marker materialized before falling back to literal BF.
                machine.write_cell(original, 0);
                machine.write_cell(machine.pointer, 1);
                return false;
            }
            let current = machine.pointer;
            let target = (current as isize + self.stride) as usize;
            if machine.tape[target] != 0 {
                // The literal loop consumes its marker, leaves the found cell
                // unchanged, and exits one stride before that cell. Any final
                // pointer movement following the loop remains ordinary code.
                machine.write_cell(original, 0);
                machine.pc = self.next;
                return true;
            }
            // Every intermediate target is an original zero and the path is
            // monotonic/in bounds. Its temporary one is consumed by the next
            // iteration before BF can observe it. Delay those cancelling
            // writes, retaining only the marker at a budget or bounds stop.
            machine.pointer = target;
        }
        // This is exactly the loop-head state after SCAN_LIMIT iterations.
        // Leave PC unchanged so a long search can resume in the next dispatch.
        machine.write_cell(original, 0);
        machine.write_cell(machine.pointer, 1);
        true
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::engine::{Error, Stop};

    fn movement(stride: isize) -> String {
        if stride > 0 {
            ">".repeat(stride as usize)
        } else {
            "<".repeat(stride.unsigned_abs())
        }
    }

    fn pattern(stride: isize) -> String {
        let forward = movement(stride);
        let reverse = movement(-stride);
        format!("[{forward}[{reverse}-]{reverse}[-{forward}+{reverse}]{forward}]")
    }

    fn finish(machine: &mut Machine, program: &Program) -> Result<Stop, Error> {
        for _ in 0..2000 {
            match machine.run(program, 1024)? {
                Stop::Budget => {}
                stop => return Ok(stop),
            }
        }
        panic!("bounded search regression did not terminate");
    }

    #[test]
    fn searches_in_both_directions_preserve_all_other_cells() {
        for stride in [-8, -3, -1, 1, 3, 8] {
            for zeros in [0, 1, 7, 31, 600] {
                for value in [1, 2, 32768, 65535] {
                    let source = format!("{}{}!", pattern(stride), movement(stride));
                    let literal = Program::parse(&source, false, true).unwrap();
                    let optimized = Program::parse(&source, true, true).unwrap();
                    assert!(Search::parse(&literal, 0).is_some());
                    let start = if stride < 0 { 6000 } else { 16 };
                    let target = (start as isize + stride * (zeros + 1)) as usize;
                    let mut expected = Machine::new(7000).unwrap();
                    expected.write_cell(start, 1);
                    expected.write_cell(target, value);
                    expected.pointer = start;
                    let mut actual = Machine::new(7000).unwrap();
                    actual.write_cell(start, 1);
                    actual.write_cell(target, value);
                    actual.pointer = start;
                    assert_eq!(finish(&mut expected, &literal).unwrap(), Stop::Hook(b'!'));
                    assert_eq!(finish(&mut actual, &optimized).unwrap(), Stop::Hook(b'!'));
                    assert_eq!(actual.pointer, target);
                    assert_eq!(actual.pointer, expected.pointer);
                    assert_eq!(actual.tape, expected.tape);
                }
            }
        }
    }

    #[test]
    fn long_search_yields_with_a_real_marker_and_resumes() {
        let source = format!("{}>!", pattern(1));
        let program = Program::parse(&source, true, true).unwrap();
        let search = Search::parse(&program, 0).unwrap();
        let mut machine = Machine::new(2000).unwrap();
        machine.pointer = 8;
        machine.write_cell(8, 1);
        machine.write_cell(1000, 17);
        assert!(search.execute(&mut machine));
        assert_eq!(machine.pc, 0);
        assert_eq!(machine.pointer, 8 + SCAN_LIMIT);
        assert_eq!(machine.tape[machine.pointer], 1);
        assert!(machine.tape[8..machine.pointer].iter().all(|&v| v == 0));
        assert_eq!(finish(&mut machine, &program).unwrap(), Stop::Hook(b'!'));
        assert_eq!(machine.pointer, 1000);
        assert_eq!(machine.tape[1000], 17);
        assert!(machine.tape[..1000].iter().all(|&v| v == 0));
    }

    #[test]
    fn noncanonical_markers_and_nonzero_trailing_cell_fall_back_unchanged() {
        let program = Program::parse(&pattern(2), false, false).unwrap();
        let search = Search::parse(&program, 0).unwrap();
        for (marker, trailing) in [(0, 0), (2, 0), (65535, 0), (1, 3)] {
            let mut machine = Machine::new(20).unwrap();
            machine.pointer = 4;
            machine.write_cell(4, marker);
            machine.write_cell(2, trailing);
            let before = machine.tape.clone();
            assert!(!search.execute(&mut machine));
            assert_eq!(machine.pointer, 4);
            assert_eq!(machine.pc, 0);
            assert_eq!(machine.tape, before);
        }
    }

    #[test]
    fn strict_errors_and_wrapping_aliases_match_literal_execution() {
        for stride in [-8, -1, 1, 8] {
            for wrapping in [false, true] {
                let source = format!("{}{}!", pattern(stride), movement(stride));
                let literal = Program::parse(&source, false, true).unwrap();
                let optimized = Program::parse(&source, true, true).unwrap();
                let start = if stride < 0 { 0 } else { 16 };
                let target = (start as isize + stride).rem_euclid(17) as usize;
                let make = || {
                    let mut machine = if wrapping {
                        Machine::with_wrapping_tape(17).unwrap()
                    } else {
                        Machine::new(17).unwrap()
                    };
                    machine.pointer = start;
                    machine.write_cell(start, 1);
                    machine.write_cell(target, 7);
                    machine
                };
                let mut expected = make();
                let mut actual = make();
                assert_eq!(
                    finish(&mut actual, &optimized),
                    finish(&mut expected, &literal)
                );
                assert_eq!(actual.pointer, expected.pointer);
                assert_eq!(actual.tape, expected.tape);
            }
        }
    }

    #[test]
    fn hooks_or_changed_arithmetic_prevent_the_fold() {
        let source = pattern(2);
        for altered in [
            source.replacen('-', "+", 1),
            source.replacen('+', "++", 1),
            source.replacen("[", "[!", 1),
        ] {
            let program = Program::parse(&altered, false, true).unwrap();
            assert!(Search::parse(&program, 0).is_none());
        }
    }

    #[test]
    fn deferred_marker_is_materialized_on_a_partial_bounds_fallback() {
        for stride in [-3, -1, 1, 3] {
            let program = Program::parse(&pattern(stride), false, false).unwrap();
            let search = Search::parse(&program, 0).unwrap();
            let mut machine = Machine::new(100).unwrap();
            let start = if stride < 0 { 96 } else { 3 };
            machine.pointer = start;
            machine.write_cell(start, 1);
            assert!(!search.execute(&mut machine));
            assert_eq!(machine.pc, 0);
            assert_ne!(machine.pointer, start);
            assert_eq!(machine.tape[machine.pointer], 1);
            assert_eq!(machine.tape.iter().filter(|&&v| v != 0).count(), 1);
            let mut expected = Machine::new(100).unwrap();
            expected.pointer = start;
            expected.write_cell(start, 1);
            // The optimized rejection is a complete loop-head state. Resuming
            // both paths must produce the same literal partial-error state.
            assert_eq!(
                finish(&mut machine, &program),
                finish(&mut expected, &program)
            );
            assert_eq!(machine.pointer, expected.pointer);
            assert_eq!(machine.tape, expected.tape);
        }
    }

    #[test]
    fn deferred_search_writes_match_literal_across_budget_mutations() {
        let source = format!("{}>!", pattern(1));
        let optimized = Program::parse(&source, true, true).unwrap();
        let literal = Program::parse(&source, false, true).unwrap();
        let mut machine = Machine::new(2000).unwrap();
        machine.pointer = 8;
        machine.write_cell(8, 1);
        machine.write_cell(1500, 13);
        assert_eq!(machine.run(&optimized, 1).unwrap(), Stop::Budget);
        assert_eq!(machine.pointer, 8 + SCAN_LIMIT);
        machine.write_cell(machine.pointer + 7, 65535);
        let mut expected = Machine::new(2000).unwrap();
        expected.pointer = machine.pointer;
        expected.tape.copy_from_slice(machine.tape());
        assert_eq!(
            finish(&mut machine, &optimized),
            finish(&mut expected, &literal)
        );
        assert_eq!(machine.pointer, expected.pointer);
        assert_eq!(machine.tape, expected.tape);
    }
}
