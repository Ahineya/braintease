//! Reuse facts learned by ordinary BF scans. Each cache contains runs satisfying
//! one predicate (nonzero for scans, zero for marker searches) at a fixed stride.
//! The exit cell is always checked rather than cached as a result.
//! Storage and work are bounded independently of tape size. No CPU layout is
//! involved, and losing a cached run affects performance only.

use super::{Machine, SCAN_LIMIT};

const SLOTS: usize = 32;

#[derive(Clone, Copy)]
pub(super) struct Run {
    low: usize,
    high: usize,
    residue: usize,
}

impl Run {
    const EMPTY: Self = Self {
        low: 1,
        high: 0,
        residue: 0,
    };

    pub(super) fn contains(self, index: usize) -> bool {
        self.low <= index && index <= self.high
    }

    pub(super) fn steps_from(self, index: usize, stride: isize) -> usize {
        if stride > 0 {
            (self.high - index) / stride as usize + 1
        } else {
            (index - self.low) / stride.unsigned_abs() + 1
        }
    }
}

pub(super) struct Cache {
    stride: usize,
    mask: Option<usize>,
    runs: [Run; SLOTS],
}

impl Default for Cache {
    fn default() -> Self {
        Self {
            stride: 0,
            mask: None,
            runs: [Run::EMPTY; SLOTS],
        }
    }
}

impl Cache {
    pub(super) fn reset(&mut self) {
        // Initialize entries lazily when a scan actually needs them.
        self.stride = 0;
    }

    fn residue(&self, index: usize) -> usize {
        match self.mask {
            Some(mask) => index & mask,
            None => index % self.stride,
        }
    }

    /// Query an existing fact without replacing a different stride's cache.
    pub(super) fn known_run(&self, stride: usize, index: usize) -> Option<Run> {
        if self.stride != stride || stride == 0 {
            return None;
        }
        let residue = self.residue(index);
        let run = self.runs[residue % SLOTS];
        (run.residue == residue && run.contains(index)).then_some(run)
    }

    pub(super) fn prepare(&mut self, stride: usize, index: usize) -> (usize, Run) {
        if self.stride != stride {
            self.stride = stride;
            self.mask = stride.is_power_of_two().then_some(stride - 1);
            self.runs.fill(Run::EMPTY);
        }
        let residue = self.residue(index);
        let slot = residue % SLOTS;
        if self.runs[slot].residue != residue {
            self.runs[slot] = Run {
                residue,
                ..Run::EMPTY
            };
        }
        (slot, self.runs[slot])
    }

    /// A deferred constant progression is already a proof of its predicate.
    /// Reuse it after a different scan stride evicts this small predicate cache,
    /// instead of probing each logical cell and searching the overlay again.
    pub(super) fn prepare_with_uniform(
        &mut self,
        tape: &super::tape::Tape,
        stride: isize,
        index: usize,
        zero: bool,
    ) -> (usize, Run) {
        let (slot, known) = self.prepare(stride.unsigned_abs(), index);
        if !known.contains(index) {
            if let Some((count, value)) = tape.uniform_prefix(index, stride, usize::MAX) {
                if (value == 0) == zero {
                    let last = (index as isize + (count - 1) as isize * stride) as usize;
                    self.remember(slot, index, last);
                }
            }
        }
        (slot, self.runs[slot])
    }

    pub(super) fn remember(&mut self, slot: usize, first: usize, last: usize) {
        let low = first.min(last);
        let high = first.max(last);
        let run = &mut self.runs[slot];
        if run.low <= run.high
            && low <= run.high.saturating_add(self.stride)
            && run.low <= high.saturating_add(self.stride)
        {
            run.low = run.low.min(low);
            run.high = run.high.max(high);
        } else {
            run.low = low;
            run.high = high;
        }
    }

    #[inline]
    pub(super) fn invalidate(&mut self, index: usize) {
        if self.stride == 0 {
            return;
        }
        let residue = self.residue(index);
        let run = &mut self.runs[residue % SLOTS];
        if run.residue != residue || !run.contains(index) {
            return;
        }
        // Retain the larger side of an interrupted run. All discarded facts
        // can be learned again; never bridge a cell violating this predicate.
        if index - run.low >= run.high - index && index > run.low {
            run.high = index - self.stride;
        } else if index < run.high {
            run.low = index + self.stride;
        } else {
            *run = Run {
                residue,
                ..Run::EMPTY
            };
        }
    }

    /// Forget a complete progression of changed cells, without repeatedly
    /// splitting the same cached run. Differing lattices conservatively reset
    /// the cache; equal strides retain other residues and the larger safe side.
    pub(super) fn invalidate_run(&mut self, first: usize, last: usize, stride: usize) {
        if self.stride == 0 {
            return;
        }
        if self.stride != stride {
            self.reset();
            return;
        }
        let residue = self.residue(first);
        let run = &mut self.runs[residue % SLOTS];
        let low = first.min(last);
        let high = first.max(last);
        if run.residue != residue || low > run.high || high < run.low {
            return;
        }
        let before = low.saturating_sub(run.low);
        let after = run.high.saturating_sub(high);
        if before >= after && before != 0 {
            run.high = low - stride;
        } else if after != 0 {
            run.low = high + stride;
        } else {
            *run = Run {
                residue,
                ..Run::EMPTY
            };
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::engine::{Error, Program, Stop};

    #[test]
    fn deferred_predicates_survive_cache_eviction_in_both_directions() {
        for direction in [-1isize, 1] {
            for stride in [8isize, 32] {
                let stride = stride * direction;
                let first = if direction > 0 { 32 } else { 65536 };
                let last = if direction > 0 { 65536 } else { 32 };
                let source = format!(
                    "[{}]$",
                    if stride > 0 { ">" } else { "<" }.repeat(stride.unsigned_abs())
                );
                let program = Program::parse(&source, true, true).unwrap();
                let mut machine = Machine::new(65580).unwrap();
                assert!(machine.tape.assign_uniform(first, last, 8, 0, 7));
                // Populate a different stride, forcing the proof to come from
                // the logical tape rather than the separate scan cache.
                machine.scan_cache.prepare(3, 1);
                machine.pointer = first;
                assert_eq!(machine.run(&program, 2).unwrap(), Stop::Hook(b'$'));
                assert_eq!(machine.pointer, (last as isize + stride) as usize);
                assert_eq!(machine.read_cell(first), 7);
                assert_eq!(machine.read_cell(last), 7);

                // An explicit mutation splits the proof. The new zero must
                // stop a later scan even after another cache eviction.
                let hole = (first as isize + 128 * stride) as usize;
                machine.write_cell(hole, 0);
                machine.scan_cache.prepare(3, 1);
                machine.pointer = first;
                machine.pc = 0;
                assert_eq!(machine.run(&program, 2).unwrap(), Stop::Hook(b'$'));
                assert_eq!(machine.pointer, hole);
            }
        }
    }

    #[test]
    fn deferred_scan_facts_preserve_cancelled_excursion_bounds() {
        let mut machine = Machine::new(1025).unwrap();
        assert!(machine.tape.assign_uniform(0, 1024, 1, 0, 1));
        let program = Program::parse("[>>><<]", true, false).unwrap();
        let failure = machine.run(&program, 16).unwrap_err();
        assert!(matches!(failure, Error::TapeBounds { pointer: 1024, .. }));
        assert_eq!(machine.pointer, 1024);
        assert_eq!(machine.read_cell(1024), 1);
    }

    #[test]
    fn readonly_route_probes_reuse_logical_zero_and_nonzero_facts() {
        let mut machine = Machine::new(65580).unwrap();
        assert!(machine.tape.assign_uniform(32, 65536, 8, 0, 7));
        let mut probes = 3;
        assert_eq!(machine.trace_scan(32, 32, 0, 32, &mut probes), Some(65568));
        assert!(probes > 0);
        // This representation now restores physical zeros, but its retained
        // clean fact is equally useful after the zero cache was evicted.
        assert!(machine.tape.assign_uniform(32, 65536, 8, 7, 0));
        machine.write_cell(65568, 19);
        machine.zero_cache.prepare(3, 1);
        let mut probes = 3;
        assert_eq!(machine.trace_zeros(32, 32, &mut probes), Some(65568));
        assert!(probes > 0);
    }

    #[test]
    fn readonly_lookup_checks_stride_residue_and_bounds_without_evicting() {
        let mut cache = Cache::default();
        assert!(cache.known_run(0, 0).is_none());
        let (slot, _) = cache.prepare(65, 65);
        cache.remember(slot, 65, 650);
        assert!(cache.known_run(1, 65).is_none());
        assert!(cache.known_run(65, 97).is_none()); // Same slot, different residue.
        assert!(cache.known_run(65, 0).is_none());
        assert!(cache.known_run(65, 715).is_none());
        assert!(cache.known_run(65, 325).is_some());
        cache.invalidate(325);
        assert!(cache.known_run(65, 325).is_none());
        cache.reset();
        assert!(cache.known_run(65, 650).is_none());
    }

    #[test]
    fn strided_invalidation_never_retains_changed_cells_or_discards_other_residues() {
        for stride in [1usize, 2, 3, 8, 37, 65] {
            for start in 0..16 {
                for length in 1..20 {
                    for reverse in [false, true] {
                        let mut cache = Cache::default();
                        let residue = stride - 1;
                        let (slot, _) = cache.prepare(stride, residue);
                        cache.remember(slot, stride * 4 + residue, stride * 24 + residue);
                        let first = start * stride + residue;
                        let last = (start + length - 1) * stride + residue;
                        if reverse {
                            cache.invalidate_run(last, first, stride);
                        } else {
                            cache.invalidate_run(first, last, stride);
                        }
                        let (_, run) = cache.prepare(stride, residue);
                        for i in 0..40 {
                            let index = i * stride + residue;
                            if run.contains(index) {
                                assert!((4..=24).contains(&i));
                                assert!(i < start || i >= start + length);
                            }
                        }
                    }
                }
            }
            if stride > 1 {
                let mut cache = Cache::default();
                let (slot, _) = cache.prepare(stride, 0);
                cache.remember(slot, stride, stride * 10);
                cache.invalidate_run(stride + 1, stride * 10 + 1, stride);
                assert!(cache.prepare(stride, 0).1.contains(stride * 5));
                cache.invalidate_run(stride, stride * 10, stride + 1);
                assert!(!cache.prepare(stride, 0).1.contains(stride * 5));
            }
        }
    }
}

impl Machine {
    /// Resolve a scan without changing the pointer or tape. Used while proving
    /// a larger arithmetic loop; its caller bounds the total probes over every
    /// scan and rejects the proof if it cannot reach a sentinel within them.
    pub(super) fn trace_scan(
        &mut self,
        mut pointer: usize,
        stride: isize,
        minimum: isize,
        maximum: isize,
        probes: &mut usize,
    ) -> Option<usize> {
        let step = stride.unsigned_abs();
        let (slot, known) = self
            .scan_cache
            .prepare_with_uniform(&self.tape, stride, pointer, false);
        let first = pointer;
        let mut last = None;
        let result = loop {
            if *probes == 0 {
                break None;
            }
            *probes -= 1;
            if self.tape[pointer] == 0 {
                break Some(pointer);
            }
            last = Some(pointer);
            if minimum.unsigned_abs() > pointer || maximum as usize >= self.tape.len() - pointer {
                break None;
            }
            if known.contains(pointer) {
                let (known_steps, safe_steps) = if stride > 0 {
                    (
                        (known.high - pointer) / step + 1,
                        (self.tape.len() - 1 - pointer - maximum as usize) / step + 1,
                    )
                } else {
                    (
                        (pointer - known.low) / step + 1,
                        (pointer - minimum.unsigned_abs()) / step + 1,
                    )
                };
                let count = known_steps.min(safe_steps);
                if stride > 0 {
                    last = Some(pointer + (count - 1) * step);
                    pointer += count * step;
                } else {
                    last = Some(pointer - (count - 1) * step);
                    pointer -= count * step;
                }
            } else {
                pointer = (pointer as isize + stride) as usize;
            }
        };
        if let Some(last) = last {
            self.scan_cache.remember(slot, first, last);
        }
        result
    }

    pub(super) fn scan(
        &mut self,
        stride: isize,
        minimum: isize,
        maximum: isize,
        next: usize,
    ) -> bool {
        let step = stride.unsigned_abs();
        let (slot, known) =
            self.scan_cache
                .prepare_with_uniform(&self.tape, stride, self.pointer, false);
        let first = self.pointer;
        let mut last = None;
        let mut success = true;
        for _ in 0..SCAN_LIMIT {
            if self.tape[self.pointer] == 0 {
                self.pc = next;
                break;
            }
            last = Some(self.pointer);
            if !self.range_fits(minimum, maximum) {
                success = false;
                break;
            }
            if known.contains(self.pointer) {
                // The first body is safe. For a monotonic sequence of starting
                // pointers, one extreme bounds the remaining complete bodies.
                // Include excursions beyond the net stride, e.g. [>><].
                let (known_steps, safe_steps) = if stride > 0 {
                    (
                        (known.high - self.pointer) / step + 1,
                        (self.tape.len() - 1 - self.pointer - maximum as usize) / step + 1,
                    )
                } else {
                    (
                        (self.pointer - known.low) / step + 1,
                        (self.pointer - minimum.unsigned_abs()) / step + 1,
                    )
                };
                let count = known_steps.min(safe_steps);
                if stride > 0 {
                    last = Some(self.pointer + (count - 1) * step);
                    self.pointer += count * step;
                } else {
                    last = Some(self.pointer - (count - 1) * step);
                    self.pointer -= count * step;
                }
            } else {
                self.pointer = (self.pointer as isize + stride) as usize;
            }
        }
        if let Some(last) = last {
            self.scan_cache.remember(slot, first, last);
        }
        // Keep the loop's opening instruction while yielding a long scan. A
        // cache hit may skip many iterations, but takes constant host work.
        if self.tape[self.pointer] == 0 {
            self.pc = next;
        }
        success
    }
}
