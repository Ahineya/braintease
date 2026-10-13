//! A generic moving-marker search for the next nonzero cell.
//!
//! The loop `[>[<-]<[->+<]>]` moves a one through zero cells to its right.
//! Mirroring each move searches left; repeating each move searches a fixed stride.
//! Its fast path requires that marker to be one and the cell behind it zero.
//! No CPU layout or guest instruction information is involved.

use super::{Machine, Op, Program, SCAN_LIMIT};

#[derive(Debug)]
pub(super) struct Search {
    pub(super) stride: isize,
    pub(super) minimum: isize,
    pub(super) maximum: isize,
    pub(super) next: usize,
}

impl Machine {
    /// Find a nonzero cell on unchanged tape for a larger route proof. The
    /// caller checks the marker and its trailing cell separately. A monotonic
    /// path to an in-bounds result then covers every search body excursion.
    pub(super) fn trace_zeros(
        &mut self,
        mut pointer: usize,
        stride: isize,
        probes: &mut usize,
    ) -> Option<usize> {
        let step = stride.unsigned_abs();
        let (slot, known) = self
            .zero_cache
            .prepare_with_uniform(&self.tape, stride, pointer, true);
        let first = pointer;
        let mut last = None;
        let result = loop {
            if *probes == 0 {
                break None;
            }
            *probes -= 1;
            if self.tape[pointer] != 0 {
                break Some(pointer);
            }
            last = Some(pointer);
            let safe_steps = if stride > 0 {
                (self.tape.len() - 1 - pointer) / step
            } else {
                pointer / step
            };
            if safe_steps == 0 {
                break None;
            }
            let count = if known.contains(pointer) {
                known.steps_from(pointer, stride).min(safe_steps)
            } else {
                1
            };
            if stride > 0 {
                last = Some(pointer + (count - 1) * step);
                pointer += count * step;
            } else {
                last = Some(pointer - (count - 1) * step);
                pointer -= count * step;
            }
        };
        if let Some(last) = last {
            self.zero_cache.remember(slot, first, last);
        }
        result
    }
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
        let step = self.stride.unsigned_abs();
        let first_target = (original as isize + self.stride) as usize;
        let (slot, known) =
            machine
                .zero_cache
                .prepare_with_uniform(&machine.tape, self.stride, first_target, true);
        for _ in 0..SCAN_LIMIT {
            if !machine.range_fits(self.minimum, self.maximum) {
                // Previous iterations are complete BF loop iterations, with
                // the marker materialized before falling back to literal BF.
                machine.write_tape(original, 0);
                machine.write_tape(machine.pointer, 1);
                let last_zero = (machine.pointer as isize - self.stride) as usize;
                machine.zero_cache.remember(slot, behind, last_zero);
                return false;
            }
            let current = machine.pointer;
            let target = (current as isize + self.stride) as usize;
            if machine.tape[target] != 0 {
                // The literal loop consumes its marker, leaves the found cell
                // unchanged, and exits one stride before that cell. Any final
                // pointer movement following the loop remains ordinary code.
                machine.write_tape(original, 0);
                machine.zero_cache.remember(slot, behind, current);
                machine.pc = self.next;
                return true;
            }
            // Every intermediate target is an original zero and the path is
            // monotonic/in bounds. Its temporary one is consumed by the next
            // iteration before BF can observe it. Delay those cancelling
            // writes, retaining only the marker at a budget or bounds stop.
            if known.contains(target) {
                // Each skipped iteration starts one stride before its zero
                // target. Bound the last complete body's excursion as well
                // as the cached interval; never jump across a tape edge.
                let safe_steps = if self.stride > 0 {
                    (machine.tape.len() - 1 - current - self.maximum as usize) / step + 1
                } else {
                    (current - self.minimum.unsigned_abs()) / step + 1
                };
                let count = known.steps_from(target, self.stride).min(safe_steps);
                machine.pointer = if self.stride > 0 {
                    current + count * step
                } else {
                    current - count * step
                };
            } else {
                machine.pointer = target;
            }
        }
        // This is exactly a loop-head state after SCAN_LIMIT bounded probes.
        // Cached probes may represent many iterations with constant host work.
        // Leave PC unchanged so a long search can resume in the next dispatch.
        machine.write_tape(original, 0);
        machine.write_tape(machine.pointer, 1);
        let last_zero = (machine.pointer as isize - self.stride) as usize;
        machine.zero_cache.remember(slot, behind, last_zero);
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

    #[test]
    fn represented_zero_runs_skip_cold_marker_searches_without_changing_tape() {
        for stride in [-32isize, -8, 8, 32] {
            let mut machine = Machine::new(65664).unwrap();
            for index in (64..=65536).step_by(8) {
                machine.write_cell(index, 5);
            }
            assert!(machine.tape.assign_uniform(64, 65536, 8, 5, 0));
            let first = if stride > 0 { 64 } else { 65536 };
            let last = if stride > 0 { 65536 } else { 64 };
            let start = (first as isize - stride) as usize;
            let stop = (last as isize + stride) as usize;
            machine.write_cell(start, 1);
            machine.write_cell(stop, 19);
            machine.pointer = start;
            machine.zero_cache.prepare(3, 1);
            let program = Program::parse(&format!("{}$", pattern(stride)), true, true).unwrap();
            assert_eq!(machine.run(&program, 2).unwrap(), Stop::Hook(b'$'));
            assert_eq!(machine.pointer, last);
            assert_eq!(machine.read_cell(start), 0);
            assert_eq!(machine.read_cell(stop), 19);
            assert!((64..=65536)
                .step_by(8)
                .all(|index| machine.read_cell(index) == 0));
        }
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
        assert!((8..machine.pointer).all(|index| machine.read_cell(index) == 0));
        assert_eq!(finish(&mut machine, &program).unwrap(), Stop::Hook(b'!'));
        assert_eq!(machine.pointer, 1000);
        assert_eq!(machine.tape[1000], 17);
        assert!((0..1000).all(|index| machine.read_cell(index) == 0));
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

    #[test]
    fn cached_zero_runs_accelerate_reverse_searches_without_changing_tape() {
        for stride in [1isize, 3, 8, 37, 65] {
            let start = stride as usize * 2;
            let stop = start + stride as usize * 1024;
            let source = format!(
                "{}${}[-]+{}$",
                pattern(stride),
                movement(stride),
                pattern(-stride)
            );
            let program = Program::parse(&source, true, true).unwrap();
            let literal = Program::parse(&source, false, true).unwrap();
            let make = || {
                let mut machine = Machine::new(stop + stride as usize + 1).unwrap();
                machine.pointer = start;
                machine.write_cell(start, 1);
                machine.write_cell(0, 7);
                machine.write_cell(stop, 19);
                machine
            };
            let mut actual = make();
            let mut expected = make();
            assert_eq!(
                finish(&mut actual, &program),
                finish(&mut expected, &literal)
            );
            assert_eq!(actual.tape, expected.tape);
            let before = actual.dispatches();
            assert_eq!(
                finish(&mut actual, &program),
                finish(&mut expected, &literal)
            );
            assert_eq!(actual.pointer, expected.pointer);
            assert_eq!(actual.tape, expected.tape);
            assert!(
                actual.dispatches() - before <= 6,
                "warm reverse scan must reuse zero run"
            );
        }
    }

    #[test]
    fn cached_zeros_observe_all_write_forms_and_mutable_tape_access() {
        let program = Program::parse(&pattern(1), true, false).unwrap();
        let literal = Program::parse(&pattern(1), false, false).unwrap();
        for write in ["host", "slice", "+", "--", "transfer", "input"] {
            let mut actual = Machine::new(2050).unwrap();
            actual.pointer = 2;
            actual.write_cell(2, 1);
            actual.write_cell(2048, 9);
            assert_eq!(finish(&mut actual, &program).unwrap(), Stop::Halted);
            actual.pointer = 512;
            actual.pc = 0;
            match write {
                "host" => actual.write_cell(512, 32768),
                "slice" => actual.tape_mut()[512] = 32768,
                "input" => {
                    let input = Program::parse(",", true, false).unwrap();
                    assert_eq!(actual.run(&input, 1).unwrap(), Stop::Input);
                    actual.provide_input(17).unwrap();
                }
                _ => {
                    let source = if write == "transfer" {
                        actual.pointer = 511;
                        actual.write_cell(511, 1);
                        "[->+<]"
                    } else {
                        write
                    };
                    let writing = Program::parse(source, true, false).unwrap();
                    assert_eq!(finish(&mut actual, &writing).unwrap(), Stop::Halted);
                }
            }
            actual.pointer = 2;
            actual.pc = 0;
            actual.write_cell(2, 1);
            let mut expected = Machine::new(actual.tape.len()).unwrap();
            expected.pointer = actual.pointer;
            expected.tape.copy_from_slice(actual.tape());
            assert_eq!(
                finish(&mut actual, &program),
                finish(&mut expected, &literal),
                "{write}"
            );
            assert_eq!(actual.pointer, 511, "{write}");
            assert_eq!(actual.pointer, expected.pointer);
            assert_eq!(actual.tape, expected.tape);
        }
    }

    #[test]
    fn warm_zero_runs_do_not_hide_removed_stops_or_boundary_fallbacks() {
        for stride in [-8isize, -1, 1, 8] {
            for wrapping in [false, true] {
                let source = pattern(stride);
                let program = Program::parse(&source, true, false).unwrap();
                let literal = Program::parse(&source, false, false).unwrap();
                let start = if stride > 0 { 16 } else { 2032 };
                let stop = if stride > 0 { 2032 } else { 16 };
                let mut actual = if wrapping {
                    Machine::with_wrapping_tape(2049)
                } else {
                    Machine::new(2049)
                }
                .unwrap();
                actual.pointer = start;
                actual.write_cell(start, 1);
                actual.write_cell(stop, 19);
                // A sentinel across the wrapping boundary makes those cases
                // terminate after the original per-character fallback wraps.
                actual.write_cell(if stride > 0 { 0 } else { 2048 }, 7);
                assert_eq!(finish(&mut actual, &program).unwrap(), Stop::Halted);
                actual.write_cell(stop, 0);
                actual.write_cell(start, 1);
                actual.pointer = start;
                actual.pc = 0;
                let mut expected = if wrapping {
                    Machine::with_wrapping_tape(2049)
                } else {
                    Machine::new(2049)
                }
                .unwrap();
                expected.pointer = start;
                expected.tape.copy_from_slice(actual.tape());
                assert_eq!(
                    finish(&mut actual, &program),
                    finish(&mut expected, &literal)
                );
                assert_eq!(actual.pointer, expected.pointer);
                assert_eq!(actual.tape, expected.tape);
            }
        }
    }
}
