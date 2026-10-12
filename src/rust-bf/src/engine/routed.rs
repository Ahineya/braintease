//! Affine loops whose destinations are found by invariant, ordinary BF scans.
//!
//! For example, `[->>[>]>+<<[<]<]` transfers its control cell into a remote
//! destination when the intervening scan lanes have suitable sentinels. Resolve
//! the route without writing, then prove that no write can change any predicate
//! along it. That proof makes every iteration follow the same route. There are
//! no assumed strides, tape layouts, sentinels, or input values. A separate
//! guarded summary handles counted first-zero filling; all other changing
//! routes retain the original BF instructions.

use super::{Machine, Op, Program, BLOCK_LIMIT, SCAN_LIMIT};

mod fill;

#[derive(Debug)]
enum Kind {
    Add(u16),
    Move(isize),
    Scan {
        stride: isize,
        minimum: isize,
        maximum: isize,
    },
}

#[derive(Debug)]
struct Part {
    kind: Kind,
}

#[derive(Debug)]
pub(super) struct Routed {
    parts: Vec<Part>,
    next: usize,
    fill: Option<fill::Fill>,
}

#[derive(Clone, Copy, Default)]
struct Change {
    index: usize,
    delta: u16,
}

#[derive(Clone, Copy, Default)]
struct Path {
    first: usize,
    last: usize,
    stride: usize,
}

impl Path {
    fn contains(self, index: usize) -> bool {
        let low = self.first.min(self.last);
        let high = self.first.max(self.last);
        low <= index && index <= high && (index - low) % self.stride == 0
    }
}

impl Routed {
    pub(super) fn parse(program: &Program, at: usize) -> Option<Self> {
        let Op::Open(next) = program.code[at].op else {
            return None;
        };
        let end = next - 1;
        if end - at > BLOCK_LIMIT {
            return None;
        }
        let mut parts = Vec::new();
        let mut cursor = at + 1;
        let mut scans = 0;
        while cursor < end {
            let kind = match program.code[cursor].op {
                Op::Add(value) => Kind::Add(value),
                Op::Move(delta) => Kind::Move(delta),
                Op::Open(after) => {
                    let scan = program.arithmetic(cursor + 1, after - 1)?;
                    if !scan.changes.is_empty() || scan.advance == 0 {
                        return None;
                    }
                    parts.push(Part {
                        kind: Kind::Scan {
                            stride: scan.advance,
                            minimum: scan.minimum,
                            maximum: scan.maximum,
                        },
                    });
                    scans += 1;
                    cursor = after;
                    continue;
                }
                _ => return None,
            };
            parts.push(Part { kind });
            cursor += 1;
        }
        let fill = fill::Fill::parse(&parts);
        (scans != 0).then_some(Self { parts, next, fill })
    }

    pub(super) fn resumes(&self) -> bool {
        self.fill.is_some()
    }

    pub(super) fn execute(&self, machine: &mut Machine) -> bool {
        if let Some(fill) = &self.fill {
            if fill.execute(self, machine) {
                return true;
            }
        }
        self.execute_affine(machine)
    }

    fn execute_affine(&self, machine: &mut Machine) -> bool {
        let start = machine.pointer;
        let value = machine.tape[start];
        if value == 0 {
            machine.pc = self.next;
            return true;
        }
        // Keep all writes, even cancelling ones: a transient update could
        // otherwise change an intervening scan before it is cancelled again.
        let mut changes = [Change::default(); BLOCK_LIMIT];
        let mut paths = [Path::default(); BLOCK_LIMIT];
        let (mut change_count, mut path_count) = (0, 0);
        let mut pointer = start;
        let mut probes = SCAN_LIMIT;
        for part in &self.parts {
            match part.kind {
                Kind::Add(delta) => {
                    if let Some(change) = changes[..change_count]
                        .iter_mut()
                        .find(|c| c.index == pointer)
                    {
                        change.delta = change.delta.wrapping_add(delta);
                    } else {
                        changes[change_count] = Change {
                            index: pointer,
                            delta,
                        };
                        change_count += 1;
                    }
                }
                Kind::Move(delta) => {
                    let Some(target) = pointer.checked_add_signed(delta) else {
                        return false;
                    };
                    if target >= machine.tape.len() {
                        return false;
                    }
                    pointer = target;
                }
                Kind::Scan {
                    stride,
                    minimum,
                    maximum,
                    ..
                } => {
                    let Some(target) =
                        machine.trace_scan(pointer, stride, minimum, maximum, &mut probes)
                    else {
                        return false;
                    };
                    paths[path_count] = Path {
                        first: pointer,
                        last: target,
                        stride: stride.unsigned_abs(),
                    };
                    path_count += 1;
                    pointer = target;
                }
            }
        }
        if pointer != start {
            return false;
        }
        let changes = &changes[..change_count];
        if paths[..path_count]
            .iter()
            .any(|path| changes.iter().any(|change| path.contains(change.index)))
        {
            return false;
        }
        let delta = changes
            .iter()
            .find(|change| change.index == start)
            .map_or(0, |change| change.delta);
        if delta & 1 == 0 {
            return false;
        }
        let mut inverse = 1u16;
        for _ in 0..4 {
            inverse = inverse.wrapping_mul(2u16.wrapping_sub(delta.wrapping_mul(inverse)));
        }
        let iterations = value.wrapping_neg().wrapping_mul(inverse);
        for change in changes {
            machine.write_cell(
                change.index,
                machine.tape[change.index].wrapping_add(change.delta.wrapping_mul(iterations)),
            );
        }
        machine.pc = self.next;
        true
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::engine::{tests::compare, Stop};

    #[test]
    fn scanned_transfer_folds_all_iterations_and_preserves_other_cells() {
        let source = "[->>[>]>+<<[<]<]";
        for value in [0, 1, 31, 32768, 65535] {
            let initial = [value, 0, 1, 2, 65535, 0, 65530];
            if value < 32768 {
                compare(source, &initial, false, false);
            }
            let program = Program::parse(source, true, false).unwrap();
            let mut machine = Machine::new(initial.len()).unwrap();
            machine.tape_mut().copy_from_slice(&initial);
            assert_eq!(machine.run(&program, 1).unwrap(), Stop::Halted);
            assert_eq!(
                machine.tape(),
                &[0, 0, 1, 2, 65535, 0, 65530u16.wrapping_add(value)]
            );
            assert_eq!(machine.pointer(), 0);
        }
    }

    #[test]
    fn scan_routes_require_no_fixed_stride_direction_or_marker_value() {
        for stride in [-7isize, -2, -1, 1, 2, 7] {
            let mv = |n: isize| {
                if n >= 0 {
                    ">".repeat(n as usize)
                } else {
                    "<".repeat(n.unsigned_abs())
                }
            };
            let start = if stride < 0 { 50 } else { 0 };
            let source = format!(
                "{}[---{}[{}]{}+++{}[{}]{}]",
                mv(start),
                mv(2 * stride),
                mv(stride),
                mv(stride),
                mv(-2 * stride),
                mv(-stride),
                mv(-stride)
            );
            for value in [0, 3, 21, 255] {
                let mut initial = vec![5; 51];
                initial[start as usize] = value;
                for at in [1, 5] {
                    initial[(start + at * stride) as usize] = 0;
                }
                compare(&source, &initial, false, false);
            }
        }
    }

    #[test]
    fn changing_any_scan_predicate_rejects_the_fold_without_writes() {
        for source in [
            "[->+[>]>+<<[<]<]",
            "[->>[>]+>+<<[<]<]",
            "[->>+[>]>+<<[<]<]",
            "[->>+->[>]>+<<<[<]<]",
        ] {
            let program = Program::parse(source, true, false).unwrap();
            let candidate = Routed::parse(&program, 0).unwrap();
            let mut machine = Machine::new(9).unwrap();
            machine
                .tape_mut()
                .copy_from_slice(&[3, 0, 1, 1, 1, 0, 4, 0, 0]);
            let before = machine.tape().to_vec();
            assert!(!candidate.execute_affine(&mut machine), "{source}");
            assert_eq!(machine.tape(), before);
            assert_eq!(machine.pointer(), 0);
            assert_eq!(machine.pc, 0);
        }
    }

    #[test]
    fn errors_wrapping_aliases_and_scan_excursions_keep_literal_state() {
        for source in [
            "[->>[>]>+<<[<]<]",
            "[->>[>><]>+<<[<]<]",
            "[->>[>]>+<<[<<>]<]",
        ] {
            for initial in [vec![3, 0, 1], vec![3, 0, 1, 0], vec![0], vec![1]] {
                compare(source, &initial, false, false);
            }
            compare(source, &[2, 0, 1, 0, 0], true, false);
        }
        // Here the route wraps onto the control cell, cancelling its decrement.
        // It is nonterminating BF, so a fold must not manufacture a zero/halt.
        let program = Program::parse("[->>[>]>+<<[<]<]", true, false).unwrap();
        let mut machine = Machine::with_wrapping_tape(4).unwrap();
        machine.tape_mut().copy_from_slice(&[2, 0, 1, 0]);
        assert_eq!(machine.run(&program, 10_000).unwrap(), Stop::Budget);
    }

    #[test]
    fn hooks_io_and_mutating_inner_loops_are_never_hidden() {
        for source in [
            "[->>[>]>+!<<[<]<]",
            "[->>[>]>+.<<[<]<]",
            "[->>[>]>+,<<[<]<]",
            "[->>[+>]>+<<[<]<]",
        ] {
            let program = Program::parse(source, true, true).unwrap();
            assert!(Routed::parse(&program, 0).is_none());
        }
    }

    #[test]
    fn cold_long_routes_are_bounded_and_cannot_hide_host_mutation() {
        let source = "[->>[>]>+<<[<]<]!";
        let program = Program::parse(source, true, true).unwrap();
        let candidate = Routed::parse(&program, 0).unwrap();
        let mut machine = Machine::new(2051).unwrap();
        machine.tape_mut().fill(1);
        machine.tape_mut()[1] = 0;
        machine.tape_mut()[2049] = 0;
        let before = machine.tape().to_vec();
        assert!(!candidate.execute_affine(&mut machine));
        assert_eq!(machine.tape(), before);
        assert_eq!(machine.pointer(), 0);
        // A newly inserted stop changes the route. A failed proof has no
        // remembered writes or destinations that could bypass this new value.
        machine.tape_mut()[9] = 0;
        let actual = machine.tape().to_vec();
        compare(source, &actual, false, true);
    }
}
