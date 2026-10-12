//! Reuse facts learned by ordinary BF scans. A run contains only known nonzero
//! cells at a particular stride; it never caches the value of the exit cell.
//! Storage and work are bounded independently of tape size. No CPU layout is
//! involved, and losing a cached run affects performance only.

use super::{Machine, SCAN_LIMIT};

const SLOTS: usize = 32;

#[derive(Clone, Copy)]
struct Run {
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

    fn contains(self, index: usize) -> bool {
        self.low <= index && index <= self.high
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

    fn prepare(&mut self, stride: usize, index: usize) -> (usize, Run) {
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

    fn remember(&mut self, slot: usize, first: usize, last: usize) {
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
    pub(super) fn zero(&mut self, index: usize) {
        if self.stride == 0 {
            return;
        }
        let residue = self.residue(index);
        let run = &mut self.runs[residue % SLOTS];
        if run.residue != residue || !run.contains(index) {
            return;
        }
        // Retain the larger side of an interrupted run. All discarded facts
        // can be learned again by subsequent scans; never bridge the new zero.
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
