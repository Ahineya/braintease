//! Logical constant progressions with a bounded, ordinary-cell backing store.
//! This is independent of BF programs, strides, layouts and marker values.
//! Bulk observations explicitly materialize; point observations see logical data.
use std::ops::{Index, IndexMut};

pub(super) const RUN_LIMIT: usize = 128;
pub(super) const MATERIALIZE_LIMIT: usize = 1024;
const MIN_RUN: usize = 64;
// An operation begins with at most RUN_LIMIT facts. The largest fallback
// touches FILL_LIMIT distinct cells, each splitting at most one run, then
// writes its control. Engine maintenance resumes before the next operation.
const MAX_RUNS: usize = RUN_LIMIT + super::FILL_LIMIT + 2;

#[derive(Clone, Copy, Debug)]
struct Run {
    low: usize,
    high: usize,
    step: usize,
    value: u16,
    /// A proved constant in the underlying cells, permitting exact cancellation.
    backing: Option<u16>,
}
impl Run {
    #[inline]
    fn contains(self, index: usize) -> bool {
        self.low <= index && index <= self.high && (index - self.low) % self.step == 0
    }
    fn count(self) -> usize {
        (self.high - self.low) / self.step + 1
    }
    fn same_lattice(self, other: Self) -> bool {
        self.step == other.step && self.low % self.step == other.low % other.step
    }
    fn may_intersect(self, other: Self) -> bool {
        if self.low > other.high || other.low > self.high {
            return false;
        }
        let (mut a, mut b) = (self.step, other.step);
        while b != 0 {
            (a, b) = (b, a % b);
        }
        self.low % a == other.low % a
    }
}

/// Conservative range/residue rejection, derived solely from the progressions.
/// Splitting or removing runs may leave false positives, never false negatives.
#[derive(Clone, Copy, Debug)]
struct Filter {
    low: usize,
    high: usize,
    residues: u64,
}
impl Default for Filter {
    fn default() -> Self {
        Self {
            low: usize::MAX,
            high: 0,
            residues: 0,
        }
    }
}
impl Filter {
    fn include(&mut self, run: Run) {
        self.low = self.low.min(run.low);
        self.high = self.high.max(run.high);
        // gcd(step, 64) gives every possible residue of this progression.
        // Odd strides conservatively cover all residues; no tape layout is assumed.
        let spacing = 1u32 << run.step.trailing_zeros().min(6);
        let pattern = if spacing == 64 {
            1
        } else {
            u64::MAX / ((1u64 << spacing) - 1)
        };
        self.residues |= pattern.rotate_left((run.low % spacing as usize) as u32);
    }
    #[inline]
    fn contains(self, index: usize) -> bool {
        self.low <= index && index <= self.high && self.residues & (1u64 << (index & 63)) != 0
    }
}

#[derive(Clone, Debug)]
pub(super) struct Tape {
    cells: Vec<u16>,
    runs: Vec<Run>,
    all: Filter,
    dirty: Filter,
}
impl Tape {
    pub(super) fn new(cells: Vec<u16>) -> Self {
        Self {
            cells,
            runs: Vec::new(),
            all: Filter::default(),
            dirty: Filter::default(),
        }
    }
    fn refresh_filters(&mut self) {
        self.all = Filter::default();
        self.dirty = Filter::default();
        for &run in &self.runs {
            self.all.include(run);
            if run.backing != Some(run.value) {
                self.dirty.include(run);
            }
        }
    }
    pub(super) fn len(&self) -> usize {
        self.cells.len()
    }
    pub(super) fn needs_materialization(&self) -> bool {
        self.runs.len() > RUN_LIMIT
    }

    /// Commit at most one bounded prefix. Logical contents never change.
    pub(super) fn materialize_step(&mut self) {
        let Some((slot, _)) = self
            .runs
            .iter()
            .enumerate()
            .min_by_key(|(_, run)| run.count())
        else {
            return;
        };
        let run = self.runs[slot];
        if run.backing == Some(run.value) {
            self.runs.swap_remove(slot);
            self.refresh_filters();
            return;
        }
        let count = run.count().min(MATERIALIZE_LIMIT);
        for n in 0..count {
            self.cells[run.low + n * run.step] = run.value;
        }
        if count == run.count() {
            self.runs.swap_remove(slot);
        } else {
            self.runs[slot].low += count * run.step;
        }
        self.refresh_filters();
    }
    pub(super) fn materialize(&mut self) -> &mut [u16] {
        self.all = Filter::default();
        self.dirty = Filter::default();
        for run in self.runs.drain(..) {
            if run.backing == Some(run.value) {
                continue;
            }
            for index in (run.low..=run.high).step_by(run.step) {
                self.cells[index] = run.value;
            }
        }
        &mut self.cells
    }

    /// Return the constant prefix already represented on this exact lattice.
    pub(super) fn uniform_prefix(
        &self,
        first: usize,
        stride: isize,
        limit: usize,
    ) -> Option<(usize, u16)> {
        if !self.all.contains(first) {
            return None;
        }
        self.runs.iter().find_map(|run| {
            if run.step != stride.unsigned_abs() || !run.contains(first) {
                return None;
            }
            let distance = if stride > 0 {
                run.high - first
            } else {
                first - run.low
            };
            Some(((distance / run.step + 1).min(limit), run.value))
        })
    }

    /// Record a progression whose previous logical value was proved uniform.
    /// Incompatible intersecting lattices use the caller's bounded point path.
    pub(super) fn assign_uniform(
        &mut self,
        first: usize,
        last: usize,
        step: usize,
        old: u16,
        value: u16,
    ) -> bool {
        let mut incoming = Run {
            low: first.min(last),
            high: first.max(last),
            step,
            value,
            backing: Some(old),
        };
        debug_assert!(step > 0 && incoming.high < self.len());
        debug_assert_eq!((incoming.high - incoming.low) % step, 0);
        if self.runs.iter().any(|run| {
            run.backing != Some(run.value)
                && !run.same_lattice(incoming)
                && run.may_intersect(incoming)
        }) {
            return false;
        }
        let covering = self.runs.iter().find(|run| {
            run.same_lattice(incoming) && run.low <= incoming.low && run.high >= incoming.high
        });
        if let Some(run) = covering {
            incoming.backing = run.backing;
        } else if self.runs.iter().any(|run| {
            run.same_lattice(incoming) && run.may_intersect(incoming) && run.backing != Some(old)
        }) {
            incoming.backing = None;
        }
        // Small isolated writes remain physical; cancellation of an existing
        // large progression is useful even when its consumed prefix is short.
        if incoming.count() < MIN_RUN && covering.is_none() {
            return false;
        }
        // A clean fact has no outstanding physical stores. Forget conflicting
        // lattices only after all rejection checks, so failed assignments are
        // atomic and successful assignments preserve non-overlapping runs.
        self.runs
            .retain(|run| run.same_lattice(incoming) || !run.may_intersect(incoming));
        let mut at = 0;
        while at < self.runs.len() {
            let run = self.runs[at];
            if !run.same_lattice(incoming) || !run.may_intersect(incoming) {
                at += 1;
                continue;
            }
            self.runs.swap_remove(at);
            if run.low < incoming.low {
                self.runs.push(Run {
                    high: incoming.low - step,
                    ..run
                });
            }
            if run.high > incoming.high {
                self.runs.push(Run {
                    low: incoming.high + step,
                    ..run
                });
            }
        }
        // Restoring the physical baseline cancels pending writes. Retain the
        // clean uniform fact, so a later fill need not read the entire range
        // again after unrelated loops evict the separate predicate caches.
        // Equal adjacent segments can be represented by one interval, including
        // when a point write has previously split the progression in two.
        let mut at = 0;
        while at < self.runs.len() {
            let run = self.runs[at];
            if run.same_lattice(incoming)
                && run.value == value
                && run.backing == incoming.backing
                && run.low <= incoming.high.saturating_add(step)
                && incoming.low <= run.high.saturating_add(step)
            {
                incoming.low = incoming.low.min(run.low);
                incoming.high = incoming.high.max(run.high);
                self.runs.swap_remove(at);
                at = 0;
            } else {
                at += 1;
            }
        }
        self.runs.push(incoming);
        self.refresh_filters();
        debug_assert!(self.runs.len() <= MAX_RUNS);
        true
    }

    /// Remove one logical point from its interval, preserving the other cells.
    fn physical_cell(&mut self, index: usize) -> &mut u16 {
        assert!(index < self.cells.len());
        if !self.all.contains(index) {
            return &mut self.cells[index];
        }
        if let Some(slot) = self.runs.iter().position(|run| run.contains(index)) {
            let run = self.runs.swap_remove(slot);
            self.cells[index] = run.value;
            if index > run.low {
                self.runs.push(Run {
                    high: index - run.step,
                    ..run
                });
            }
            if index < run.high {
                self.runs.push(Run {
                    low: index + run.step,
                    ..run
                });
            }
        }
        debug_assert!(self.runs.len() <= MAX_RUNS);
        &mut self.cells[index]
    }

    #[cfg(test)]
    pub(super) fn iter(&self) -> impl Iterator<Item = &u16> {
        (0..self.len()).map(|index| &self[index])
    }
    #[cfg(test)]
    pub(super) fn copy_from_slice(&mut self, source: &[u16]) {
        self.runs.clear();
        self.refresh_filters();
        self.cells.copy_from_slice(source);
    }
}
impl Index<usize> for Tape {
    type Output = u16;
    #[inline]
    fn index(&self, index: usize) -> &u16 {
        assert!(index < self.cells.len());
        // Clean facts already agree with physical cells. Most ordinary reads
        // need no search, even while unrelated marker runs remain deferred.
        if !self.dirty.contains(index) {
            return &self.cells[index];
        }
        self.runs
            .iter()
            .find(|run| run.backing != Some(run.value) && run.contains(index))
            .map_or(&self.cells[index], |run| &run.value)
    }
}
impl IndexMut<usize> for Tape {
    fn index_mut(&mut self, index: usize) -> &mut u16 {
        self.physical_cell(index)
    }
}
#[cfg(test)]
impl PartialEq for Tape {
    fn eq(&self, other: &Self) -> bool {
        self.len() == other.len() && self.iter().eq(other.iter())
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn filters_never_reject_a_progression_point_for_any_residue() {
        for step in 1..=257 {
            for low in 0..64 {
                let run = Run {
                    low,
                    high: low + 128 * step,
                    step,
                    value: 9,
                    backing: Some(0),
                };
                let mut filter = Filter::default();
                filter.include(run);
                for index in (run.low..=run.high).step_by(step) {
                    assert!(
                        filter.contains(index),
                        "step={step}, low={low}, index={index}"
                    );
                }
                assert!(!filter.contains(run.high + 1));
            }
        }
    }
    #[test]
    fn clean_reads_and_unrelated_residues_skip_deferred_lookup() {
        let mut tape = Tape::new(vec![0; 4096]);
        assert!(tape.assign_uniform(8, 4000, 8, 0, 7));
        assert!(tape.dirty.contains(800));
        assert!(!tape.dirty.contains(801));
        tape[801] = 19;
        tape[800] = 23;
        assert_eq!(tape[792], 7);
        assert_eq!(tape[800], 23);
        assert_eq!(tape[801], 19);
        tape.materialize();
        assert!(!tape.dirty.contains(792));
        assert!(tape.assign_uniform(8, 792, 8, 7, 7));
        assert!(tape.all.contains(792));
        assert!(!tape.dirty.contains(792));
        tape[792] = 0;
        assert_eq!(tape[792], 0);
        assert_eq!(tape[784], 7);
    }
    #[test]
    fn constant_runs_cancel_without_touching_backing_and_coalesce_both_directions() {
        let mut tape = Tape::new(vec![0; 10000]);
        for first in (8..8000).step_by(512) {
            assert!(tape.assign_uniform(first, first + 504, 8, 0, 65535));
        }
        assert_eq!(tape.runs.len(), 1);
        assert!(tape.cells.iter().all(|&v| v == 0));
        let high = tape.runs[0].high;
        assert_eq!(tape[high], 65535);
        for first in (8..=high).step_by(512) {
            assert!(tape.assign_uniform(first, first + 504, 8, 65535, 0));
        }
        assert!(tape.runs.iter().all(|run| run.backing == Some(run.value)));
        assert!(tape.cells.iter().all(|&v| v == 0));
    }
    #[test]
    fn point_splits_and_mixed_lattice_fallback_preserve_logical_values() {
        let mut tape = Tape::new(vec![7; 3000]);
        assert!(tape.assign_uniform(0, 2040, 8, 7, 3));
        tape[800] = 65535;
        assert_eq!(tape[792], 3);
        assert_eq!(tape[800], 65535);
        assert_eq!(tape[808], 3);
        assert!(!tape.assign_uniform(0, 2040, 3, 3, 0));
        let expected: Vec<_> = tape.iter().copied().collect();
        assert_eq!(tape.materialize(), expected);
        assert!(tape.runs.is_empty());
    }
    #[test]
    fn clean_facts_can_be_replaced_by_an_intersecting_lattice() {
        let mut tape = Tape::new(vec![0; 4096]);
        assert!(tape.assign_uniform(0, 4088, 8, 0, 1));
        assert!(tape.assign_uniform(0, 4088, 8, 1, 0));
        assert!(tape.assign_uniform(0, 4064, 32, 0, 7));
        assert!(tape.cells.iter().all(|&value| value == 0));
        for index in 0..4096 {
            assert_eq!(tape[index], if index % 32 == 0 { 7 } else { 0 });
        }
        assert!(tape.assign_uniform(0, 4064, 32, 7, 0));
        assert!(tape.assign_uniform(0, 4088, 8, 0, 9));
        assert!(tape.cells.iter().all(|&value| value == 0));
        for index in 0..4096 {
            assert_eq!(tape[index], if index % 8 == 0 { 9 } else { 0 });
        }
    }

    #[test]
    fn dirty_lattice_conflicts_reject_without_removing_clean_facts() {
        let mut tape = Tape::new(vec![0; 4096]);
        assert!(tape.assign_uniform(0, 2040, 8, 0, 0));
        for index in (2048..=4088).step_by(8) {
            tape[index] = 5;
        }
        assert!(tape.assign_uniform(2048, 4088, 8, 5, 0));
        let before = format!("{:?}", tape.runs);
        // Both facts may intersect the incoming stride. The pending dirty
        // stores require the ordinary point path; even the clean fact stays.
        assert!(!tape.assign_uniform(0, 4064, 32, 0, 7));
        assert_eq!(format!("{:?}", tape.runs), before);
    }
    #[test]
    fn materialization_is_bounded_even_for_a_large_progression() {
        let mut tape = Tape::new(vec![0; 50000]);
        assert!(tape.assign_uniform(0, 49999, 1, 0, 9));
        tape.materialize_step();
        assert_eq!(
            tape.cells.iter().filter(|&&v| v == 9).count(),
            MATERIALIZE_LIMIT
        );
        assert!(tape.iter().all(|&v| v == 9));
    }
}

#[cfg(test)]
mod execution_tests {
    use super::*;
    use crate::engine::{Machine, Profile, Program, Stop};

    #[test]
    fn proven_fill_and_cleanup_cancel_a_large_run_across_a_hook() {
        let mut machine = Machine::new(65540).unwrap();
        machine.write_cell(0, 64000);
        let (slot, _) = machine.zero_cache.prepare(1, 2);
        machine.zero_cache.remember(slot, 2, 65539);
        let program = Program::parse("[->>[>]+[<]<]$>>[>]<[-<]!", true, true).unwrap();
        assert_eq!(machine.run(&program, 2).unwrap(), Stop::Hook(b'$'));
        assert_eq!(machine.tape.runs.len(), 1);
        assert_eq!(machine.read_cell(64001), 1);
        assert!(machine.tape.cells.iter().all(|&v| v == 0));
        let before = machine.dispatches();
        assert_eq!(machine.run(&program, 16).unwrap(), Stop::Hook(b'!'));
        assert!(machine.dispatches() - before <= 6);
        assert!(machine
            .tape
            .runs
            .iter()
            .all(|run| run.backing == Some(run.value)));
        assert!(machine.tape.cells.iter().all(|&v| v == 0));
    }

    #[test]
    fn disjoint_predicate_walk_and_cleanup_update_large_constant_runs() {
        for direction in [-1isize, 1] {
            let count = 64000;
            let mut machine = Machine::new(count * 8 + 32).unwrap();
            let stride = direction * 8;
            let first = if direction > 0 { 16 } else { count * 8 + 8 };
            let last = (first as isize + (count - 1) as isize * stride) as usize;
            let target = (first as isize + direction * 2) as usize;
            let target_last = (last as isize + direction * 2) as usize;
            for index in (first.min(last)..=first.max(last)).step_by(8) {
                machine.write_cell(index, 1);
            }
            let (slot, _) = machine.scan_cache.prepare(8, first);
            machine.scan_cache.remember(slot, first, last);
            let (slot, _) = machine.zero_cache.prepare(8, target);
            machine.zero_cache.remember(slot, target, target_last);
            machine.pointer = first;
            let movement = if direction > 0 { ">" } else { "<" };
            let source = format!("[{}++{}]$", movement.repeat(2), movement.repeat(6));
            let program = Program::parse(&source, true, true).unwrap();
            assert_eq!(machine.run(&program, 2).unwrap(), Stop::Hook(b'$'));
            assert_eq!(machine.tape.runs.len(), 1);
            for index in (target.min(target_last)..=target.max(target_last)).step_by(8) {
                assert_eq!(machine.read_cell(index), 2);
                assert_eq!(machine.tape.cells[index], 0);
            }
            machine.pointer = target;
            machine.pc = 0;
            let cleanup =
                Program::parse(&format!("[--{}]$", movement.repeat(8)), true, true).unwrap();
            assert_eq!(machine.run(&cleanup, 2).unwrap(), Stop::Hook(b'$'));
            for index in (target.min(target_last)..=target.max(target_last)).step_by(8) {
                assert_eq!(machine.read_cell(index), 0);
                assert_eq!(machine.tape.cells[index], 0);
            }
        }
    }

    #[test]
    fn physical_walks_learn_clean_runs_that_survive_predicate_cache_eviction() {
        for direction in [-1isize, 1] {
            let count = 2048;
            let mut machine = Machine::new(count * 8 + 32).unwrap();
            let stride = direction * 8;
            let first = if direction > 0 { 16 } else { count * 8 + 8 };
            let last = (first as isize + (count - 1) as isize * stride) as usize;
            let target = (first as isize + direction * 2) as usize;
            let target_last = (last as isize + direction * 2) as usize;
            for index in (first.min(last)..=first.max(last)).step_by(8) {
                machine.write_cell(index, 1);
            }
            let movement = if direction > 0 { ">" } else { "<" };
            let program = Program::parse(
                &format!("[{}+{}]$", movement.repeat(2), movement.repeat(6)),
                true,
                true,
            )
            .unwrap();
            machine.pointer = first;
            assert_eq!(machine.run(&program, 100).unwrap(), Stop::Hook(b'$'));
            assert_eq!(
                machine.tape.uniform_prefix(target, stride, count),
                Some((count, 1))
            );
            assert!(machine
                .tape
                .runs
                .iter()
                .all(|run| run.backing == Some(run.value)));

            // A real scan relearns the invariant predicates after both caches
            // were discarded. The independent destination fact must survive.
            machine.scan_cache.reset();
            machine.zero_cache.reset();
            machine.pointer = first;
            machine.pc = 0;
            let scan = Program::parse(&format!("[{}]", movement.repeat(8)), true, false).unwrap();
            assert_eq!(machine.run(&scan, 100).unwrap(), Stop::Halted);
            machine.pointer = first;
            machine.pc = 0;
            assert_eq!(machine.run(&program, 2).unwrap(), Stop::Hook(b'$'));
            for index in (target.min(target_last)..=target.max(target_last)).step_by(8) {
                assert_eq!(machine.read_cell(index), 2);
                assert_eq!(machine.tape.cells[index], 1);
            }

            // Remove the old tape fact via the explicit raw-view barrier, then
            // execute a completely cold current-cell walk. Its committed zero
            // outputs also become a reusable clean fact.
            machine.tape_mut();
            machine.pointer = target;
            machine.pc = 0;
            let cleanup =
                Program::parse(&format!("[--{}]$", movement.repeat(8)), true, true).unwrap();
            assert_eq!(machine.run(&cleanup, 100).unwrap(), Stop::Hook(b'$'));
            assert_eq!(
                machine.tape.uniform_prefix(target, stride, count),
                Some((count, 0))
            );
            assert!(machine
                .tape
                .runs
                .iter()
                .all(|run| run.backing == Some(run.value)));
        }
    }

    #[test]
    fn overlay_pressure_consumes_bounded_profiled_dispatches_and_preserves_state() {
        let mut machine = Machine::new(30000).unwrap();
        for n in 0..RUN_LIMIT + 12 {
            let low = 128 + n * 128;
            assert!(machine.tape.assign_uniform(low, low + 63, 1, 0, 9));
        }
        let program = Program::parse("+", true, false).unwrap();
        let mut profile = Profile::default();
        for _ in 0..12 {
            let before = machine.tape.cells.iter().filter(|&&v| v != 0).count();
            assert_eq!(
                machine.run_profiled(&program, 1, &mut profile).unwrap(),
                Stop::Budget
            );
            let after = machine.tape.cells.iter().filter(|&&v| v != 0).count();
            assert!(after - before <= MATERIALIZE_LIMIT);
            assert_eq!(machine.pc, 0);
            assert_eq!(machine.read_cell(128), 9);
            assert_eq!(machine.read_cell(128 + (RUN_LIMIT + 11) * 128), 9);
        }
        assert_eq!(
            machine.run_profiled(&program, 1, &mut profile).unwrap(),
            Stop::Halted
        );
        assert_eq!(machine.read_cell(0), 1);
        let report = profile.report();
        assert_eq!(report.dispatches, 13);
        assert_eq!(
            report
                .operations
                .iter()
                .map(|operation| operation.attempts)
                .sum::<u64>(),
            13
        );
        assert_eq!(
            report
                .operations
                .iter()
                .find(|operation| operation.kind == "materialize")
                .unwrap()
                .attempts,
            12
        );
    }

    #[test]
    fn randomized_uniform_assignments_splits_materialization_and_points_match_dense_tape() {
        let mut tape = Tape::new(vec![0; 4096]);
        let mut expected = vec![0u16; 4096];
        let mut seed = 0x749c_2631u32;
        let mut random = || {
            seed ^= seed << 13;
            seed ^= seed >> 17;
            seed ^= seed << 5;
            seed
        };
        for turn in 0..2500 {
            let step = [1, 2, 3, 8, 17][random() as usize % 5];
            let low = random() as usize % 512;
            let count = (1 + random() as usize % 128).min((expected.len() - 1 - low) / step + 1);
            let high = low + (count - 1) * step;
            let value = [0, 1, 65535, random() as u16][random() as usize % 4];
            if turn % 7 == 0 {
                tape.materialize_step();
            }
            if turn % 31 == 0 {
                assert_eq!(tape.materialize(), expected);
            }
            let old = expected[low];
            let uniform = (low..=high)
                .step_by(step)
                .all(|index| expected[index] == old);
            if !uniform || !tape.assign_uniform(high, low, step, old, value) {
                for index in (low..=high).step_by(step) {
                    tape[index] = value;
                }
            }
            for index in (low..=high).step_by(step) {
                expected[index] = value;
            }
            let point = random() as usize % expected.len();
            tape[point] = value;
            expected[point] = value;
            assert!(
                tape.iter().copied().eq(expected.iter().copied()),
                "turn={turn}"
            );
            while tape.needs_materialization() {
                tape.materialize_step();
            }
        }
    }
}

#[cfg(test)]
#[path = "tape_model_tests.rs"]
mod model_tests;
