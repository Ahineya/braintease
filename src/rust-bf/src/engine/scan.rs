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
        let (slot, known) = self.scan_cache.prepare(step, pointer);
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
        let (slot, known) = self.scan_cache.prepare(step, self.pointer);
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
