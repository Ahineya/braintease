//! A counted first-zero fill, proved from BF operations and current tape state.
//! The changing forward/reverse scan pair encloses exactly one nonzero write.
//! All other scans must be disjoint from every changed cell. No tape layout is
//! assumed; checked batches leave every marker physically present on the tape.

use super::{Kind, Machine, Part, Path, Routed, BLOCK_LIMIT, SCAN_LIMIT};

#[derive(Debug)]
pub(super) struct Fill {
    forward: usize,
    write: usize,
    reverse: usize,
    stride: isize,
    value: u16,
}

impl Fill {
    pub(super) fn parse(parts: &[Part]) -> Option<Self> {
        if parts.iter().any(|p| matches!(p.kind, Kind::Search(_))) {
            return None;
        }
        if !matches!(parts.first()?.kind, Kind::Add(u16::MAX)) {
            return None;
        }
        let writes: Vec<_> = parts
            .iter()
            .enumerate()
            .filter_map(|(i, p)| matches!(p.kind, Kind::Add(_)).then_some(i))
            .collect();
        if writes.len() != 2 {
            return None;
        }
        let write = writes[1];
        let Kind::Add(value) = parts[write].kind else {
            return None;
        };
        if value == 0 {
            return None;
        }
        let forward = (0..write)
            .rev()
            .find(|&i| matches!(parts[i].kind, Kind::Scan { .. }))?;
        let reverse =
            (write + 1..parts.len()).find(|&i| matches!(parts[i].kind, Kind::Scan { .. }))?;
        let Kind::Scan { stride, .. } = parts[forward].kind else {
            return None;
        };
        if !matches!(parts[reverse].kind, Kind::Scan {stride: back,..} if stride.checked_neg() == Some(back))
        {
            return None;
        }
        Some(Self {
            forward,
            write,
            reverse,
            stride,
            value,
        })
    }

    pub(super) fn execute(&self, routed: &Routed, machine: &mut Machine) -> bool {
        let control = machine.pointer;
        let count = machine.tape[control];
        if count == 0 {
            machine.pc = routed.next;
            return true;
        }
        let mut pointer = control;
        let mut target = None;
        let mut paths = [Path::default(); BLOCK_LIMIT];
        let mut path_count = 0;
        let mut probes = SCAN_LIMIT;
        let (mut minimum, mut maximum) = (0isize, 0isize);
        for (index, part) in routed.parts.iter().enumerate() {
            match part.kind {
                Kind::Add(_) if index == 0 => {}
                Kind::Add(_) if index == self.write => {
                    if target != Some(pointer) || machine.tape[pointer] != 0 {
                        return false;
                    }
                }
                Kind::Add(_) => return false,
                Kind::Search(_) => return false,
                Kind::Move(delta) => {
                    let Some(next) = pointer.checked_add_signed(delta) else {
                        return false;
                    };
                    if next >= machine.tape.len() {
                        return false;
                    }
                    pointer = next;
                    if index > self.forward && index < self.reverse {
                        let offset = pointer as isize - target.unwrap() as isize;
                        minimum = minimum.min(offset);
                        maximum = maximum.max(offset);
                    }
                }
                Kind::Scan {
                    stride,
                    minimum: low,
                    maximum: high,
                    ..
                } => {
                    let first = pointer;
                    if index == self.reverse {
                        // The sole speculative marker write makes its original
                        // zero nonzero. Execute its first scan step explicitly,
                        // then scan unchanged cells using the normal cache.
                        if target != Some(pointer)
                            || low.unsigned_abs() > pointer
                            || high as usize >= machine.tape.len() - pointer
                        {
                            return false;
                        }
                        pointer = (pointer as isize + stride) as usize;
                    }
                    let Some(last) = machine.trace_scan(pointer, stride, low, high, &mut probes)
                    else {
                        return false;
                    };
                    pointer = last;
                    if index == self.forward {
                        target = Some(last);
                        minimum = minimum.min(low);
                        maximum = maximum.max(high);
                    }
                    if index == self.reverse {
                        minimum = minimum.min(low);
                        maximum = maximum.max(high);
                        let target = target.unwrap();
                        if (self.stride > 0 && last >= target)
                            || (self.stride < 0 && last <= target)
                        {
                            return false;
                        }
                    }
                    let path = Path {
                        first,
                        last,
                        stride: stride.unsigned_abs(),
                    };
                    if path.contains(control) {
                        return false;
                    }
                    // The two changing scans intentionally read the new fill
                    // cells. Every other predicate must remain invariant.
                    if index != self.forward && index != self.reverse {
                        paths[path_count] = path;
                        path_count += 1;
                    }
                }
            }
        }
        if pointer != control {
            return false;
        }
        let Some(first) = target else {
            return false;
        };
        // Bound both fill writes and disjointness checks. A nonzero cell ends
        // this batch; the next invocation resolves the next first-zero route.
        let limit = usize::from(count).min(SCAN_LIMIT / (path_count + 2));
        let mut filled = 0;
        let mut current = first;
        while filled < limit {
            if current == control
                || machine.tape[current] != 0
                || minimum.unsigned_abs() > current
                || maximum as usize >= machine.tape.len() - current
                || paths[..path_count].iter().any(|p| p.contains(current))
            {
                break;
            }
            filled += 1;
            let Some(next) = current.checked_add_signed(self.stride) else {
                break;
            };
            if next >= machine.tape.len() {
                break;
            }
            current = next;
        }
        if filled == 0 {
            return false;
        }
        current = first;
        for _ in 0..filled {
            machine.write_cell(current, self.value);
            current = (current as isize).wrapping_add(self.stride) as usize;
        }
        machine.write_cell(control, count - filled as u16);
        machine.pc = if count == filled as u16 {
            routed.next
        } else {
            routed.next - 1
        };
        true
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::engine::{tests::compare, Program, Stop};

    // Control at0, first-zero filling from2 rightwards, return over the growing
    // nonzero run to the sentinel at1. This is generic unary conversion.
    const SOURCE: &str = "[->>[>]+[<]<]";

    #[test]
    fn fills_actual_tape_cells_with_bounded_work_and_literal_state() {
        for count in [0, 1, 7, 31, 255] {
            let mut initial = vec![0; 300];
            initial[0] = count;
            compare(SOURCE, &initial, false, false);
            let program = Program::parse(SOURCE, true, false).unwrap();
            let mut machine = Machine::new(initial.len()).unwrap();
            machine.tape_mut().copy_from_slice(&initial);
            while machine.run(&program, 1).unwrap() == Stop::Budget {}
            assert_eq!(machine.tape()[0], 0);
            assert!(machine.tape()[2..2 + count as usize]
                .iter()
                .all(|&v| v == 1));
            assert!(machine.dispatches() <= 3, "{}", machine.dispatches());
        }
    }

    #[test]
    fn maximum_cell_count_fills_the_complete_physical_tape_in_bounded_slices() {
        for stride in [-2isize, -1, 1, 2] {
            let movement = |delta: isize| {
                if delta >= 0 {
                    ">".repeat(delta as usize)
                } else {
                    "<".repeat(delta.unsigned_abs())
                }
            };
            let source = format!(
                "[-{}[{}]+[{}]{}]",
                movement(2 * stride),
                movement(stride),
                movement(-stride),
                movement(-stride)
            );
            let program = Program::parse(&source, true, false).unwrap();
            let cells = 65540 * stride.unsigned_abs();
            let start = if stride > 0 { 0 } else { cells - 1 };
            let mut machine = Machine::new(cells).unwrap();
            machine.pointer = start;
            machine.write_cell(start, 65535);
            assert_eq!(machine.run(&program, 1).unwrap(), Stop::Budget);
            assert_eq!(machine.tape[start], 65535 - 128);
            while machine.run(&program, 1).unwrap() == Stop::Budget {}
            assert_eq!(machine.pointer(), start);
            assert_eq!(machine.tape[start], 0);
            assert_eq!(machine.tape.iter().filter(|&&v| v != 0).count(), 65535);
            for marker in 0..65535 {
                assert_eq!(
                    machine.tape[(start as isize + (marker + 2) * stride) as usize],
                    1
                );
            }
            assert!(machine.dispatches() <= 513);
        }
    }

    #[test]
    fn varying_strides_holes_and_marker_values_match_literal_bf() {
        for stride in [-5isize, -2, -1, 1, 2, 5] {
            let mv = |n: isize| {
                if n >= 0 {
                    ">".repeat(n as usize)
                } else {
                    "<".repeat(n.unsigned_abs())
                }
            };
            let start = if stride < 0 { 200 } else { 0 };
            for delta in ["+", "++", "---"] {
                let source = format!(
                    "{}[-{}[{}]{delta}[{}]{}]",
                    mv(start),
                    mv(2 * stride),
                    mv(stride),
                    mv(-stride),
                    mv(-stride)
                );
                let mut initial = vec![0; 201];
                initial[start as usize] = 19;
                for offset in [3, 7, 8, 12] {
                    initial[(start + offset * stride) as usize] = 32768;
                }
                compare(&source, &initial, false, false);
            }
        }
    }

    #[test]
    fn budget_mutation_rechecks_the_route_and_preserves_bounds_failures() {
        let program = Program::parse(SOURCE, true, false).unwrap();
        let mut machine = Machine::new(1000).unwrap();
        machine.write_cell(0, 500);
        assert_eq!(machine.run(&program, 1).unwrap(), Stop::Budget);
        let filled = 500 - machine.tape()[0] as usize;
        assert!(filled > 0 && filled <= SCAN_LIMIT);
        machine.write_cell(20, 0);
        while machine.run(&program, 1).unwrap() == Stop::Budget {}
        assert_eq!(machine.tape()[0], 0);
        assert!(machine.tape()[2..501].iter().all(|&v| v == 1));
        assert_eq!(machine.tape()[501], 0);
        for cells in [1, 2, 3, 9, 130] {
            let mut initial = vec![0; cells];
            initial[0] = 200;
            compare(SOURCE, &initial, false, false);
        }
    }

    #[test]
    fn changing_static_routes_or_aliasing_control_rejects_fill() {
        for source in ["[->>[>]+[<]<]", "[->>[>]+>><<[<]<]"] {
            for initial in [vec![1, 0, 0], vec![0]] {
                compare(source, &initial, true, false);
            }
            let program = Program::parse(source, true, false).unwrap();
            let mut machine = Machine::with_wrapping_tape(1).unwrap();
            machine.write_cell(0, 1);
            assert_eq!(machine.run(&program, 1000).unwrap(), Stop::Budget);
        }
        // Returning to a moving sentinel rather than the control is legal BF,
        // but cannot be summarized as a countdown at the original pointer.
        compare("[->>[>]+[<]<<<]", &[3, 0, 1, 0, 0, 0], false, false);
    }

    #[test]
    fn randomized_fill_with_stationary_routes_and_excursions_matches_literal() {
        let mut seed = 0x3742_a196u32;
        let mut random = || {
            seed ^= seed << 13;
            seed ^= seed >> 17;
            seed ^= seed << 5;
            seed
        };
        let movement = |delta: isize| {
            if delta >= 0 {
                ">".repeat(delta as usize)
            } else {
                "<".repeat(delta.unsigned_abs())
            }
        };
        for _ in 0..200 {
            let direction = if random() & 1 == 0 { 1 } else { -1 };
            let stride = direction * 2 * (1 + random() % 4) as isize;
            let start = if direction > 0 { 32 } else { 479 };
            let count = 1 + random() % 15;
            let excursion = (random() % 3) as isize;
            let scan = format!(
                "{}{}",
                movement(stride + direction * excursion),
                movement(-direction * excursion)
            );
            let reverse = format!(
                "{}{}",
                movement(-stride - direction * excursion),
                movement(direction * excursion)
            );
            // The first round trip scans an independent residue. Its predicate
            // cells must remain unchanged while the even-residue fill grows.
            let source = format!(
                "{}[-{}[{}]{}[{}]{}{}[{scan}]+[{reverse}]{}]",
                movement(start),
                movement(stride + direction),
                movement(stride),
                movement(-stride),
                movement(-stride),
                movement(-direction),
                movement(2 * stride),
                movement(-stride)
            );
            let mut initial = vec![0; 512];
            initial[start as usize] = count as u16;
            for offset in 1..=1 + random() % 8 {
                initial[(start + direction + stride * offset as isize) as usize] =
                    (random() as u16).max(1);
            }
            for offset in 2..=8 {
                if random() & 1 == 0 {
                    initial[(start + stride * offset) as usize] = (random() as u16).max(1);
                }
            }
            compare(&source, &initial, false, false);
        }
    }
}
