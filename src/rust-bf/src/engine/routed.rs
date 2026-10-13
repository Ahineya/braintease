//! Affine loops whose destinations are found by invariant, ordinary BF scans.
//!
//! For example, `[->>[>]>+<<[<]<]` transfers its control cell into a remote
//! destination when the intervening scan lanes have suitable sentinels. Resolve
//! the route without writing, then prove that no write can change any predicate
//! along it. That proof makes every iteration follow the same route. There are
//! no assumed strides, tape layouts, sentinels, or input values. A separate
//! guarded summary handles counted first-zero filling; all other changing
//! routes retain the original BF instructions.

use super::{search::Search, Machine, Op, Program, BLOCK_LIMIT, SCAN_LIMIT};

mod fill;

#[derive(Debug)]
enum Kind {
    Add(u16),
    Move(Motion),
    Scan {
        stride: isize,
        minimum: isize,
        maximum: isize,
    },
    Search(Search),
}

#[derive(Debug, Clone, Copy, Default)]
struct Motion {
    advance: isize,
    minimum: isize,
    maximum: isize,
}

impl Motion {
    fn resolve(self, pointer: usize, cells: usize) -> Option<usize> {
        // A cancelled move still has an excursion. Reject the whole readonly
        // proof if any original move crosses an edge; literal fallback then
        // preserves wrapping aliases or the exact partial state on a fault.
        if self.minimum.unsigned_abs() > pointer || self.maximum as usize >= cells - pointer {
            return None;
        }
        pointer.checked_add_signed(self.advance)
    }
}

#[derive(Debug)]
struct Part {
    kind: Kind,
}

#[derive(Debug)]
pub(super) struct Routed {
    parts: Vec<Part>,
    start: usize,
    next: usize,
    fill: Option<fill::Fill>,
    retry: bool,
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
                Op::Move(_) => {
                    let mut motion = Motion::default();
                    while cursor < end {
                        let Op::Move(delta) = program.code[cursor].op else {
                            break;
                        };
                        motion.advance = motion.advance.checked_add(delta)?;
                        motion.minimum = motion.minimum.min(motion.advance);
                        motion.maximum = motion.maximum.max(motion.advance);
                        cursor += 1;
                    }
                    parts.push(Part {
                        kind: Kind::Move(motion),
                    });
                    continue;
                }
                Op::Open(after) => {
                    if let Some(search) = Search::parse(program, cursor) {
                        cursor = search.next;
                        parts.push(Part {
                            kind: Kind::Search(search),
                        });
                        scans += 1;
                        continue;
                    }
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
        // Even a statically impossible fill shape can benefit from retrying
        // its affine route after the first literal body warms a scan cache.
        let retry = fill.is_some() || parts.iter().any(|p| matches!(p.kind, Kind::Search(_)));
        let fill = fill.and_then(|fill| fill.compile(&parts));
        (scans != 0).then_some(Self {
            parts,
            start: at,
            next,
            fill,
            retry,
        })
    }

    pub(super) fn resumes(&self) -> bool {
        self.retry
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
        let mut locations = [0usize; BLOCK_LIMIT];
        let mut change_count = 0;
        let mut pointer = start;
        let mut probes = SCAN_LIMIT;
        for (part_index, part) in self.parts.iter().enumerate() {
            locations[part_index] = pointer;
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
                Kind::Move(motion) => {
                    let Some(target) = motion.resolve(pointer, machine.tape.len()) else {
                        return false;
                    };
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
                    paths[part_index] = Path {
                        first: pointer,
                        last: target,
                        stride: stride.unsigned_abs(),
                    };
                    pointer = target;
                }
                Kind::Search(ref search) => {
                    let partial = changes[..change_count]
                        .iter()
                        .find(|c| c.index == pointer)
                        .map_or(0, |c| c.delta);
                    if machine.tape[pointer].wrapping_add(partial) != 1
                        || search.minimum.unsigned_abs() > pointer
                        || search.maximum as usize >= machine.tape.len() - pointer
                    {
                        return false;
                    }
                    let behind = (pointer as isize - search.stride) as usize;
                    if machine.tape[behind] != 0 {
                        return false;
                    }
                    let first = (pointer as isize + search.stride) as usize;
                    let Some(found) = machine.trace_zeros(first, search.stride, &mut probes) else {
                        return false;
                    };
                    paths[part_index] = Path {
                        first: behind,
                        last: found,
                        stride: search.stride.unsigned_abs(),
                    };
                    // All intermediate markers cancel. The only net write is
                    // consuming the initial one; retain this transient write
                    // even if an earlier increment makes its total delta zero.
                    if let Some(change) = changes[..change_count]
                        .iter_mut()
                        .find(|c| c.index == pointer)
                    {
                        change.delta = change.delta.wrapping_sub(1);
                    } else {
                        changes[change_count] = Change {
                            index: pointer,
                            delta: u16::MAX,
                        };
                        change_count += 1;
                    }
                    pointer = (found as isize - search.stride) as usize;
                }
            }
        }
        if pointer != start {
            return false;
        }
        let changes = &changes[..change_count];
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
        let mut prefix = iterations;
        // Replay just the writes symbolically at their resolved destinations.
        // A written scan cell is safe only when every evaluation sees the same
        // zero/nonzero result as the unmodified route used above. Include the
        // exact partial delta at each scan, not only the whole-body delta.
        let mut partial = [0u16; BLOCK_LIMIT];
        for (part_index, part) in self.parts.iter().enumerate() {
            match part.kind {
                Kind::Add(delta) => {
                    let index = changes
                        .iter()
                        .position(|c| c.index == locations[part_index])
                        .unwrap();
                    partial[index] = partial[index].wrapping_add(delta);
                }
                Kind::Scan { .. } => {
                    let path = paths[part_index];
                    for (index, change) in changes.iter().enumerate() {
                        if path.contains(change.index) {
                            prefix = prefix.min(predicate_prefix(
                                machine.tape[change.index],
                                partial[index],
                                change.delta,
                            ));
                            if prefix == 0 {
                                return false;
                            }
                        }
                    }
                }
                Kind::Search(_) => {
                    let marker = changes
                        .iter()
                        .position(|c| c.index == locations[part_index])
                        .unwrap();
                    prefix = prefix.min(equality_prefix(
                        machine.tape[changes[marker].index],
                        partial[marker],
                        changes[marker].delta,
                        1,
                    ));
                    let path = paths[part_index];
                    for (index, change) in changes.iter().enumerate() {
                        if index == marker || !path.contains(change.index) {
                            continue;
                        }
                        let limit = if change.index == path.last {
                            predicate_prefix(
                                machine.tape[change.index],
                                partial[index],
                                change.delta,
                            )
                        } else {
                            equality_prefix(
                                machine.tape[change.index],
                                partial[index],
                                change.delta,
                                0,
                            )
                        };
                        prefix = prefix.min(limit);
                    }
                    if prefix == 0 {
                        return false;
                    }
                    partial[marker] = partial[marker].wrapping_sub(1);
                }
                Kind::Move(_) => {}
            }
        }
        for change in changes {
            machine.write_tape(
                change.index,
                machine.tape[change.index].wrapping_add(change.delta.wrapping_mul(prefix)),
            );
        }
        // A partial summary has completed whole bodies, but not their final
        // backedge check. Rechecking `[` is equivalent to that `]` predicate,
        // including after host mutation at a budget yield. Re-resolve the
        // route before another summary; a changed boundary uses literal BF.
        machine.pc = if prefix == iterations {
            self.next
        } else {
            self.start
        };
        true
    }
}

fn equality_prefix(initial: u16, partial: u16, delta: u16, expected: u16) -> u16 {
    if initial.wrapping_add(partial) != expected {
        0
    } else if delta == 0 {
        u16::MAX
    } else {
        1
    }
}

fn predicate_prefix(initial: u16, partial: u16, delta: u16) -> u16 {
    let first = initial.wrapping_add(partial);
    if initial == 0 {
        return if first != 0 {
            0
        } else if delta == 0 {
            u16::MAX
        } else {
            1
        };
    }
    if first == 0 {
        return 0;
    }
    // A signed representative of the modular delta gives a conservative
    // monotone interval. Keeping both endpoints in1..65535 proves every read
    // nonzero without simulating the counter or assuming wrapping never occurs.
    // Stop before the first read that could cross either endpoint. Values
    // after the final read may wrap: they are materialized and checked afresh.
    let signed = i32::from(delta as i16);
    if signed > 0 {
        (1 + (65535 - i32::from(first)) / signed) as u16
    } else if signed < 0 {
        (1 + (i32::from(first) - 1) / -signed) as u16
    } else {
        u16::MAX
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::engine::{tests::compare, Stop};

    // Even cells route forward over nonzero values; odd cells route back over
    // zeros to a nonzero sentinel. The temporary marker cancels each body.
    const SEARCH_TRANSFER: &str = "[->>[>>]<<+>+[<<[>>-]>>[-<<+>>]<<]<<<]";

    #[test]
    fn impossible_fill_routes_skip_the_fill_proof_but_keep_affine_retries() {
        for source in ["[->>[>]>+<<[<]<]", "[->>[>]+>[<]<<]"] {
            let program = Program::parse(source, true, false).unwrap();
            let route = Routed::parse(&program, 0).unwrap();
            assert!(route.fill.is_none(), "static target mismatch: {source}");
            assert!(route.resumes(), "the affine retry remains useful: {source}");
            for initial in [vec![0], vec![3, 0, 1], vec![3, 0, 1, 1, 0, 0, 0]] {
                compare(source, &initial, false, false);
            }
        }
    }

    #[test]
    fn compiled_motions_retain_cancelled_excursions_and_literal_fault_state() {
        let source = "[->>><[>]>><<+><[<]><<]";
        let program = Program::parse(source, true, false).unwrap();
        let route = Routed::parse(&program, 0).unwrap();
        assert_eq!(route.parts.len(), 8, "combine consecutive pointer moves");
        assert!(
            route.fill.is_some(),
            "cancelled excursions still allow fills"
        );
        for cells in 1..20 {
            for count in [0, 1, 3, 12] {
                let mut initial = vec![0; cells];
                initial[0] = count;
                compare(source, &initial, false, false);
            }
        }
        compare(source, &[2, 0, 1, 0, 0], true, false);
        compare(
            &format!("{source}!"),
            &[3, 0, 0, 0, 0, 0, 0, 0],
            false,
            true,
        );
        for barrier in ["?", "!", "$", ",", "."] {
            let altered = source.replacen(">><<", &format!(">>{barrier}<<"), 1);
            let program = Program::parse(&altered, true, true).unwrap();
            assert!(Routed::parse(&program, 0).is_none());
        }
    }

    #[test]
    fn marker_searches_inside_affine_routes_fold_complete_transfers() {
        for count in [0, 1, 3, 31, 32768, 65535] {
            let initial = [count, 7, 1, 0, 1, 0, 1, 0, 1, 0, 0, 0];
            if count < 100 {
                compare(SEARCH_TRANSFER, &initial, false, false);
            }
            let program = Program::parse(SEARCH_TRANSFER, true, false).unwrap();
            let mut machine = Machine::new(initial.len()).unwrap();
            machine.tape_mut().copy_from_slice(&initial);
            assert_eq!(machine.run(&program, 1).unwrap(), Stop::Halted);
            let mut expected = initial;
            expected[0] = 0;
            expected[8] = 1u16.wrapping_add(count);
            assert_eq!(machine.tape(), expected);
            assert_eq!(machine.pointer(), 0);
        }
    }

    #[test]
    fn searched_transfers_preserve_stride_direction_and_wrapping_read_boundaries() {
        for stride in [-7isize, -3, -1, 1, 3, 7] {
            let mut source = String::new();
            for ch in SEARCH_TRANSFER.chars() {
                match ch {
                    '<' | '>' => {
                        let forward = (ch == '>') == (stride > 0);
                        source.push_str(
                            &if forward { ">" } else { "<" }.repeat(stride.unsigned_abs()),
                        );
                    }
                    _ => source.push(ch),
                }
            }
            let cells = 12 * stride.unsigned_abs();
            let start = if stride > 0 { 0 } else { cells - 1 };
            let source = format!("{}{source}", ">".repeat(start));
            for count in [0, 1, 2, 7, 31] {
                for destination in [0, 1, 65534, 65535] {
                    let mut initial = vec![0; cells];
                    for (at, value) in
                        [(0, count), (1, 7), (2, 1), (4, 1), (6, 1), (8, destination)]
                    {
                        initial[(start as isize + at * stride) as usize] = value;
                    }
                    compare(&source, &initial, false, false);
                    compare(&source, &initial, true, false);
                }
            }
        }
    }

    #[test]
    fn searched_routes_check_transient_writes_and_marker_values_before_committing() {
        for source in [
            SEARCH_TRANSFER.replacen(">+[<<", ">+<<+>>[<<", 1),
            SEARCH_TRANSFER.replacen("<<+>", "<<+>>>+<<", 1),
            SEARCH_TRANSFER.replacen(">+[<<", ">[<<", 1),
            SEARCH_TRANSFER.replacen(">+[<<", ">++[<<", 1),
            SEARCH_TRANSFER.replacen(">+[<<", ">+<<<<<<<<------->>>>>>>>[<<", 1),
        ] {
            let program = Program::parse(&source, true, false).unwrap();
            let candidate = Routed::parse(&program, 0).unwrap();
            let mut machine = Machine::new(12).unwrap();
            machine
                .tape_mut()
                .copy_from_slice(&[3, 7, 1, 0, 1, 0, 1, 0, 1, 0, 0, 0]);
            let before = machine.tape().to_vec();
            assert!(!candidate.execute_affine(&mut machine), "{source}");
            assert_eq!(machine.tape(), before);
            assert_eq!(machine.pointer(), 0);
            assert_eq!(machine.pc, 0);
            compare(&source, &before, false, false);
        }
        // A marker update that wraps to one is valid once; its net change
        // prevents folding a second body under the same exact-one guard.
        let source = SEARCH_TRANSFER.replacen(">+[<<", ">++[<<", 1);
        compare(
            &source,
            &[2, 7, 1, 0, 1, 0, 1, 0, 1, 65535, 0, 0],
            false,
            false,
        );
        // Cancelling an intermediate write before the search is safe.
        let source = SEARCH_TRANSFER.replacen(">+[<<", ">+<<+->>[<<", 1);
        compare(&source, &[7, 7, 1, 0, 1, 0, 1, 0, 1, 0, 0, 0], false, false);
    }

    #[test]
    fn searched_routes_preserve_cold_budget_boundaries_host_changes_and_fault_state() {
        let source = format!("{}!{}", SEARCH_TRANSFER, SEARCH_TRANSFER);
        compare(&source, &[3, 7, 1, 0, 1, 0, 1, 0, 1, 0, 0, 0], false, true);
        for length in [1, 2, 7, 9, 11] {
            let mut initial = vec![0; length];
            initial[0] = 3;
            for at in [1, 2, 4, 6, 8] {
                if at < length {
                    initial[at] = 1;
                }
            }
            compare(SEARCH_TRANSFER, &initial, false, false);
        }
        // Warmed zero/nonzero facts survive a failed readonly proof. The
        // executor installed at the closing bracket can try again after the
        // first literal body has learned its longer route.
        let program = Program::parse(SEARCH_TRANSFER, true, false).unwrap();
        let mut machine = Machine::new(2052).unwrap();
        machine.write_cell(0, 64000);
        machine.write_cell(1, 7);
        for at in (2..=2048).step_by(2) {
            machine.write_cell(at, 1);
        }
        assert_eq!(machine.run(&program, 1).unwrap(), Stop::Budget);
        assert_eq!(machine.tape[0], 64000);
        assert_eq!(machine.pointer(), 0);
        machine.write_cell(0, 31);
        while machine.run(&program, 7).unwrap() == Stop::Budget {}
        assert_eq!(machine.tape[0], 0);
        assert_eq!(machine.tape[2048], 32);
        assert_eq!(machine.pointer(), 0);
        assert!(machine.dispatches() < 100);
    }

    #[test]
    fn repeated_searches_use_their_own_partial_updates_and_caches() {
        let route = &SEARCH_TRANSFER[2..SEARCH_TRANSFER.len() - 1];
        let source = format!("[-{route}{route}]");
        let mut seed = 0x917c_326bu32;
        for _ in 0..100 {
            seed ^= seed << 13;
            seed ^= seed >> 17;
            seed ^= seed << 5;
            let count = 1 + (seed % 31) as u16;
            let destination = [0, 1, 32768, 65533, 65534, 65535][seed as usize % 6];
            let initial = [count, 7, 1, 0, 1, 0, 1, 0, destination, 0, 0, 0];
            compare(&source, &initial, false, false);
            compare(&source, &initial, true, false);
        }
        compare(SEARCH_TRANSFER, &[0], false, false);
        let hooked = SEARCH_TRANSFER.replacen("[<<[>>-]", "[!<<[>>-]", 1);
        let program = Program::parse(&hooked, true, true).unwrap();
        assert!(Routed::parse(&program, 0).is_none());
    }

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
    fn changed_zero_tests_reject_the_fold_without_writes() {
        for (source, scan_value) in [
            ("[->+[>]>+<<[<]<]", 1),
            ("[->>[>]+[<]<]", 1),
            ("[->>+[>]>+<<[<]<]", 65535),
        ] {
            let program = Program::parse(source, true, false).unwrap();
            let candidate = Routed::parse(&program, 0).unwrap();
            let mut machine = Machine::new(9).unwrap();
            machine
                .tape_mut()
                .copy_from_slice(&[3, 0, scan_value, 1, 1, 0, 4, 0, 0]);
            let before = machine.tape().to_vec();
            assert!(!candidate.execute_affine(&mut machine), "{source}");
            assert_eq!(machine.tape(), before);
            assert_eq!(machine.pointer(), 0);
            assert_eq!(machine.pc, 0);
        }
    }

    #[test]
    fn written_scan_cells_fold_only_while_each_iteration_preserves_the_predicate() {
        for source in [">>[-<-[<]>>]", ">>[-<+[<]>>]"] {
            for count in [0, 1, 2, 3, 17, 63] {
                for scanned in [0, 1, 2, 3, 16, 64, 32768, 65534, 65535] {
                    compare(source, &[0, scanned, count, 0, 0], false, false);
                    compare(source, &[0, scanned, count, 0, 0], true, false);
                }
            }
        }
        let source = "[-<-[<]>>]";
        let program = Program::parse(source, true, false).unwrap();
        let mut machine = Machine::new(4).unwrap();
        machine.pointer = 2;
        machine.write_cell(1, 65535);
        machine.write_cell(2, 32768);
        assert_eq!(machine.run(&program, 1).unwrap(), Stop::Halted);
        assert_eq!(machine.tape(), &[0, 32767, 0, 0]);
        assert_eq!(machine.pointer(), 2);
    }

    #[test]
    fn prefixes_materialize_whole_bodies_before_a_changed_boundary_iteration() {
        for (source, initial, prefix_tape, final_tape) in [
            (
                "[-<-[<]>>]",
                [0, 3, 7, 0, 0],
                [0, 1, 5, 0, 0],
                [0, 0, 4, 0, 0],
            ),
            (
                "[-<+[<]>>]",
                [0, 65533, 7, 0, 0],
                [0, 65535, 5, 0, 0],
                [0, 0, 4, 0, 0],
            ),
        ] {
            compare(&format!(">>{source}"), &initial, false, false);
            let program = Program::parse(source, true, false).unwrap();
            let mut machine = Machine::new(initial.len()).unwrap();
            machine.pointer = 2;
            machine.tape_mut().copy_from_slice(&initial);
            assert_eq!(machine.run(&program, 1).unwrap(), Stop::Budget);
            assert_eq!(machine.tape(), prefix_tape);
            assert_eq!(machine.pointer(), 2);
            assert_eq!(machine.pc, 0);
            let candidate = Routed::parse(&program, 0).unwrap();
            assert!(!candidate.execute_affine(&mut machine));
            assert_eq!(machine.tape(), prefix_tape);
            while machine.run(&program, 1).unwrap() == Stop::Budget {}
            assert_eq!(machine.tape(), final_tape);
            assert_eq!(machine.pointer(), 3);
        }
        // An unchanged zero exit can become nonzero after its final read. Only
        // one body is safe for that route, and the next one must find a new exit.
        compare(
            "[->>[>]+<<[<]<]",
            &[3, 0, 1, 1, 1, 0, 0, 0, 0],
            false,
            false,
        );
        compare(
            "[->>[>]+>+<<[<]<]",
            &[3, 0, 1, 1, 1, 0, 4, 0, 0],
            false,
            false,
        );
        // The changed boundary can expose a later bounds error. All preceding
        // folded bodies and the failing body's partial updates must survive.
        compare(">>[-<-[<]>>>+<]", &[0, 3, 7, 0, 0], false, false);
    }

    #[test]
    fn prefix_budget_yields_recheck_host_changes_and_wrapping_control() {
        let source = "[-<-[<]>>]";
        let program = Program::parse(source, true, false).unwrap();
        for (scanned, count) in [(12, 4), (12, 0), (0, 4), (1, 4)] {
            let mut actual = Machine::new(5).unwrap();
            actual.pointer = 2;
            actual.tape_mut().copy_from_slice(&[0, 3, 7, 0, 0]);
            assert_eq!(actual.run(&program, 1).unwrap(), Stop::Budget);
            assert_eq!(actual.tape(), &[0, 1, 5, 0, 0]);
            actual.write_cell(1, scanned);
            actual.write_cell(2, count);
            // The summary stopped before the backedge predicate. Compare a
            // literal parser resumed at `]`, including a host-zeroed control.
            let plain = Program::parse(source, false, false).unwrap();
            let mut expected = Machine::new(5).unwrap();
            expected.pointer = actual.pointer;
            expected.pc = plain.code.len() - 1;
            expected.tape_mut().copy_from_slice(actual.tape());
            let run = |machine: &mut Machine, program: &Program| loop {
                match machine.run(program, 100) {
                    Ok(Stop::Budget) => {}
                    result => break result,
                }
            };
            let got = run(&mut actual, &program);
            let want = run(&mut expected, &plain);
            assert_eq!(got, want);
            assert_eq!(actual.pointer(), expected.pointer());
            assert_eq!(actual.tape(), expected.tape());
        }
        for control in ["+", "+++", "---", "--"] {
            for count in [0, 1, 2, 3, 65535] {
                let source = format!(">>[{control}<-[<]>>]");
                compare(&source, &[0, 7, count, 0, 0], false, false);
            }
        }
        // Odd modular counters still fold large prefixes in constant work.
        let mut machine = Machine::new(5).unwrap();
        machine.pointer = 2;
        machine.tape_mut().copy_from_slice(&[0, 16384, 64000, 0, 0]);
        assert_eq!(machine.run(&program, 1).unwrap(), Stop::Budget);
        assert_eq!(machine.tape(), &[0, 1, 47617, 0, 0]);
        assert_eq!(machine.pointer(), 2);
    }

    #[test]
    fn partial_deltas_at_multiple_scans_cannot_be_replaced_with_the_net_delta() {
        let source = ">>[-<+[<]>><-[<]>>]";
        for count in [0, 1, 3, 7] {
            for scanned in [1, 2, 65534, 65535] {
                compare(source, &[0, scanned, count, 0, 0], false, false);
            }
        }
        let raw = &source[2..];
        let program = Program::parse(raw, true, false).unwrap();
        let candidate = Routed::parse(&program, 0).unwrap();
        let mut machine = Machine::new(5).unwrap();
        machine.pointer = 2;
        machine.write_cell(1, 65535);
        machine.write_cell(2, 3);
        let before = machine.tape().to_vec();
        assert!(!candidate.execute_affine(&mut machine));
        assert_eq!(machine.tape(), before);
        machine.write_cell(1, 12);
        assert!(candidate.execute_affine(&mut machine));
        assert_eq!(machine.tape(), &[0, 12, 0, 0, 0]);
        // A cancelled update before the scan keeps a zero exit safe; cancelling
        // after the scan cannot justify treating its transient one as zero.
        compare("[->+-[>]<]", &[5, 0, 0, 0], false, false);
        compare("[->+[>]-<]", &[5, 0, 0, 0], false, false);
    }

    #[test]
    fn accepted_predicate_bounds_match_all_wrapping_read_values() {
        for initial in [0u16, 1, 2, 127, 32768, 65535] {
            for partial in [0u16, 1, 127, 65534, 65535] {
                for delta in [0u16, 1, 2, 9, 32768, 65529, 65533, 65535] {
                    let prefix = predicate_prefix(initial, partial, delta);
                    let mut value = initial.wrapping_add(partial);
                    for _ in 0..prefix {
                        assert_eq!(value == 0, initial == 0);
                        value = value.wrapping_add(delta);
                    }
                }
            }
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
