//! Resumable, optimized Brainfuck execution with wrapping 16-bit cells.
//!
//! This module has no device dependencies. Input, output and optional host
//! markers are synchronous barriers: execution resumes only after the caller
//! has had an opportunity to inspect or modify the tape. Budgets and
//! `dispatches()` count IR dispatches, not original Brainfuck characters.

use std::collections::BTreeMap;
use std::fmt;

mod checkpoint;
mod profile;
mod routed;
mod scan;
mod search;
mod tape;

#[cfg(all(feature = "jit", any(target_arch = "aarch64", target_arch = "x86_64")))]
pub mod jit;
#[cfg(not(all(feature = "jit", any(target_arch = "aarch64", target_arch = "x86_64"))))]
#[path = "engine/jit/unavailable.rs"]
pub mod jit;

pub use checkpoint::CheckpointError;
pub use profile::{OperationProfile, Profile, ProfileReport, SiteProfile};

const NO_FAST_OP: u32 = u32::MAX;
const BLOCK_LIMIT: usize = 128;
const SCAN_LIMIT: usize = 256;
const FILL_LIMIT: usize = 1024;

#[derive(Debug, Clone, PartialEq, Eq)]
pub enum Error {
    Parse {
        offset: usize,
        message: &'static str,
    },
    EmptyTape,
    TapeAllocation {
        cells: usize,
    },
    TapeBounds {
        offset: usize,
        pointer: usize,
    },
    NoPendingInput,
    DispatchOverflow,
    Jit(String),
}

impl fmt::Display for Error {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::Parse { offset, message } => write!(f, "Brainfuck source byte {offset}: {message}"),
            Self::EmptyTape => write!(f, "Brainfuck tape must contain at least one cell"),
            Self::TapeAllocation { cells } => write!(f, "cannot allocate a Brainfuck tape of {cells} cells"),
            Self::TapeBounds { offset, pointer } => write!(f, "Brainfuck pointer left the tape near source byte {offset} (last valid cell {pointer})"),
            Self::NoPendingInput => write!(f, "Brainfuck machine is not awaiting input"),
            Self::DispatchOverflow => write!(f, "Brainfuck dispatch counter overflow"),
            Self::Jit(message) => write!(f, "{message}"),
        }
    }
}

impl std::error::Error for Error {}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Stop {
    Halted,
    Budget,
    Input,
    Output(u8),
    Hook(u8),
}

#[derive(Debug, Clone, Copy)]
enum Op {
    Add(u16),
    Move(isize),
    Open(usize),
    Close(usize),
    Input,
    Output,
    Hook(u8),
}

#[derive(Debug, Clone, Copy)]
struct Instruction {
    op: Op,
    offset: usize,
}

#[derive(Debug)]
struct Arithmetic {
    changes: Vec<(isize, u16)>,
    advance: isize,
    minimum: isize,
    maximum: isize,
}

#[derive(Debug)]
enum FastOp {
    Search(search::Search),
    Routed(routed::Routed),
    Block {
        arithmetic: Arithmetic,
        next: usize,
    },
    Affine {
        arithmetic: Arithmetic,
        inverse: u16,
        next: usize,
    },
    Scan {
        stride: isize,
        minimum: isize,
        maximum: isize,
        next: usize,
    },
    Walk {
        arithmetic: Arithmetic,
        next: usize,
    },
}

/// Immutable parsed code; a program can be shared by independent machines.
#[derive(Debug)]
pub struct Program {
    optimize_enabled: bool,
    host_hooks_enabled: bool,
    code: Vec<Instruction>,
    fast_at: Vec<u32>,
    fast: Vec<FastOp>,
}

impl Program {
    /// Parse ordinary BF, optionally recognizing `?`, `!` and `$` as host hooks.
    /// All other bytes are comments. Cell arithmetic always wraps modulo 65536.
    pub fn parse(source: &str, optimize: bool, host_hooks: bool) -> Result<Self, Error> {
        let mut code: Vec<Instruction> = Vec::new();
        let mut brackets: Vec<usize> = Vec::new();
        for (offset, byte) in source.bytes().enumerate() {
            let op = match byte {
                b'+' | b'-' => {
                    let delta = if byte == b'+' { 1 } else { u16::MAX };
                    if let Some(Instruction {
                        op: Op::Add(value), ..
                    }) = code.last_mut()
                    {
                        *value = value.wrapping_add(delta);
                        continue;
                    }
                    Op::Add(delta)
                }
                b'>' | b'<' => {
                    let delta = if byte == b'>' { 1isize } else { -1isize };
                    if let Some(Instruction {
                        op: Op::Move(value),
                        ..
                    }) = code.last_mut()
                    {
                        if value.signum() == delta {
                            *value = value.checked_add(delta).ok_or(Error::Parse {
                                offset,
                                message: "pointer movement is too large",
                            })?;
                            continue;
                        }
                    }
                    Op::Move(delta)
                }
                b'[' => {
                    brackets.push(code.len());
                    Op::Open(usize::MAX)
                }
                b']' => {
                    let start = brackets.pop().ok_or(Error::Parse {
                        offset,
                        message: "unmatched closing bracket",
                    })?;
                    code[start].op = Op::Open(code.len() + 1);
                    Op::Close(start + 1)
                }
                b',' => Op::Input,
                b'.' => Op::Output,
                b'?' | b'!' | b'$' if host_hooks => Op::Hook(byte),
                _ => continue,
            };
            code.push(Instruction { op, offset });
        }
        if let Some(start) = brackets.pop() {
            return Err(Error::Parse {
                offset: code[start].offset,
                message: "unmatched opening bracket",
            });
        }
        let mut program = Self {
            optimize_enabled: optimize,
            host_hooks_enabled: host_hooks,
            fast_at: vec![NO_FAST_OP; code.len()],
            code,
            fast: Vec::new(),
        };
        if optimize {
            program.optimize()?;
        }
        Ok(program)
    }

    pub fn instruction_count(&self) -> usize {
        self.code.len()
    }
    pub fn optimization_count(&self) -> usize {
        self.fast.len()
    }

    fn install(&mut self, at: usize, fast: FastOp) -> Result<(), Error> {
        if self.fast.len() >= NO_FAST_OP as usize {
            return Err(Error::Parse {
                offset: self.code[at].offset,
                message: "too many optimized operations",
            });
        }
        self.fast_at[at] = self.fast.len() as u32;
        self.fast.push(fast);
        Ok(())
    }

    fn arithmetic(&self, start: usize, end: usize) -> Option<Arithmetic> {
        if end - start > BLOCK_LIMIT {
            return None;
        }
        let mut changes = BTreeMap::<isize, u16>::new();
        let (mut advance, mut minimum, mut maximum) = (0isize, 0isize, 0isize);
        for instruction in &self.code[start..end] {
            match instruction.op {
                Op::Add(delta) => {
                    let value = changes.entry(advance).or_default();
                    *value = value.wrapping_add(delta);
                }
                Op::Move(delta) => {
                    advance = advance.checked_add(delta)?;
                    minimum = minimum.min(advance);
                    maximum = maximum.max(advance);
                }
                _ => return None,
            }
        }
        Some(Arithmetic {
            changes: changes
                .into_iter()
                .filter(|(_, delta)| *delta != 0)
                .collect(),
            advance,
            minimum,
            maximum,
        })
    }

    fn optimize(&mut self) -> Result<(), Error> {
        // Preserve the original instructions as the fallback for a fast path
        // whose complete pointer range would leave the tape. This preserves
        // wrapping aliases and the exact partial state before a bounds error.
        for at in 0..self.code.len() {
            let Op::Open(next) = self.code[at].op else {
                continue;
            };
            if let Some(search) = search::Search::parse(self, at) {
                self.install(at, FastOp::Search(search))?;
                continue;
            }
            if let Some(routed) = routed::Routed::parse(self, at) {
                let resumes = routed.resumes();
                self.install(at, FastOp::Routed(routed))?;
                // BF backedges enter the body, bypassing `[`. At `]` the
                // current predicate decides whether to execute another body,
                // so the same bounded loop executor can resume there too.
                if resumes {
                    self.fast_at[next - 1] = self.fast_at[at];
                }
                continue;
            }
            let end = next - 1;
            let Some(mut arithmetic) = self.arithmetic(at + 1, end) else {
                continue;
            };
            if arithmetic.changes.is_empty() && arithmetic.advance != 0 {
                self.install(
                    at,
                    FastOp::Scan {
                        stride: arithmetic.advance,
                        minimum: arithmetic.minimum,
                        maximum: arithmetic.maximum,
                        next,
                    },
                )?;
            } else if arithmetic.advance != 0 {
                self.install(at, FastOp::Walk { arithmetic, next })?;
            } else {
                let delta = arithmetic
                    .changes
                    .iter()
                    .find(|(offset, _)| *offset == 0)
                    .map_or(0, |(_, delta)| *delta);
                // An odd increment is invertible modulo 2^16. Even increments
                // may never reach zero, so leave those loops intact.
                if delta & 1 != 0 {
                    let mut inverse = 1u16;
                    for _ in 0..4 {
                        inverse =
                            inverse.wrapping_mul(2u16.wrapping_sub(delta.wrapping_mul(inverse)));
                    }
                    arithmetic.changes.retain(|(offset, _)| *offset != 0);
                    self.install(
                        at,
                        FastOp::Affine {
                            arithmetic,
                            inverse,
                            next,
                        },
                    )?;
                }
            }
        }
        let mut at = 0;
        while at < self.code.len() {
            if !matches!(self.code[at].op, Op::Add(_) | Op::Move(_)) {
                at += 1;
                continue;
            }
            let mut end = at + 1;
            while end < self.code.len()
                && end - at < BLOCK_LIMIT
                && matches!(self.code[end].op, Op::Add(_) | Op::Move(_))
            {
                end += 1;
            }
            if end - at > 1 {
                if let Some(arithmetic) = self.arithmetic(at, end) {
                    self.install(
                        at,
                        FastOp::Block {
                            arithmetic,
                            next: end,
                        },
                    )?;
                }
            }
            at = end;
        }
        Ok(())
    }
}

/// Mutable execution state. `new` uses fixed, checked tape bounds for native
/// hosts; `with_wrapping_tape` preserves the traditional CLI's pointer wrapping.
pub struct Machine {
    tape: tape::Tape,
    pointer: usize,
    pc: usize,
    dispatches: u64,
    pending_input: bool,
    wrapping_tape: bool,
    scan_cache: scan::Cache,
    zero_cache: scan::Cache,
}

impl Machine {
    pub fn new(tape_cells: usize) -> Result<Self, Error> {
        Self::allocate(tape_cells, false)
    }
    pub fn with_wrapping_tape(tape_cells: usize) -> Result<Self, Error> {
        Self::allocate(tape_cells, true)
    }

    fn allocate(cells: usize, wrapping_tape: bool) -> Result<Self, Error> {
        if cells == 0 {
            return Err(Error::EmptyTape);
        }
        // Offsets use isize; reject tapes that cannot be indexed by that type.
        if cells > isize::MAX as usize {
            return Err(Error::TapeAllocation { cells });
        }
        let mut tape = Vec::new();
        tape.try_reserve_exact(cells)
            .map_err(|_| Error::TapeAllocation { cells })?;
        tape.resize(cells, 0);
        Ok(Self {
            tape: tape::Tape::new(tape),
            pointer: 0,
            pc: 0,
            dispatches: 0,
            pending_input: false,
            wrapping_tape,
            scan_cache: scan::Cache::default(),
            zero_cache: scan::Cache::default(),
        })
    }

    /// Materialize all deferred writes for a coherent bulk observation.
    pub fn tape(&mut self) -> &[u16] {
        self.tape.materialize()
    }
    /// Exact logical point observation without materializing other cells.
    pub fn read_cell(&self, index: usize) -> u16 {
        self.tape[index]
    }
    pub fn tape_len(&self) -> usize {
        self.tape.len()
    }
    pub fn tape_mut(&mut self) -> &mut [u16] {
        // The caller can change any cell, including a cached scan destination.
        self.scan_cache.reset();
        self.zero_cache.reset();
        self.tape.materialize()
    }
    pub fn pointer(&self) -> usize {
        self.pointer
    }
    pub fn dispatches(&self) -> u64 {
        self.dispatches
    }

    pub fn provide_input(&mut self, value: u8) -> Result<(), Error> {
        if !self.pending_input {
            return Err(Error::NoPendingInput);
        }
        self.write_cell(self.pointer, u16::from(value));
        self.pending_input = false;
        Ok(())
    }

    /// Write one tape cell, preserving cached facts about all other cells.
    /// Prefer this to `tape_mut()` for isolated host responses. Panics if
    /// `index` is outside the allocated tape, like indexing `tape_mut()`.
    /// Explicit caller mutations may synchronously materialize deferred runs
    /// to keep storage bounded; interpreter writes use budgeted maintenance.
    #[inline]
    pub fn write_cell(&mut self, index: usize, value: u16) {
        // Explicit caller mutations have no interpreter budget. Reclaim bounded
        // overlay storage here; execution uses resumable maintenance instead.
        if self.tape.needs_materialization() {
            self.tape.materialize();
        }
        self.write_tape(index, value);
    }

    #[inline]
    fn write_tape(&mut self, index: usize, value: u16) {
        // Both predicates depend only on zero-ness, not a particular value.
        let old = self.tape[index];
        if old == value {
            return;
        }
        if value == 0 && old != 0 {
            self.scan_cache.invalidate(index);
        } else if value != 0 && old == 0 {
            self.zero_cache.invalidate(index);
        }
        self.tape[index] = value;
    }

    #[inline]
    fn range_fits(&self, minimum: isize, maximum: isize) -> bool {
        // Every summarized range includes zero; unsigned_abs handles MIN.
        minimum.unsigned_abs() <= self.pointer
            && (maximum as usize) < self.tape.len() - self.pointer
    }

    #[inline]
    fn apply(&mut self, arithmetic: &Arithmetic, multiplier: u16) {
        for &(offset, delta) in &arithmetic.changes {
            let target = (self.pointer as isize + offset) as usize;
            self.write_tape(
                target,
                self.tape[target].wrapping_add(delta.wrapping_mul(multiplier)),
            );
        }
        self.pointer = (self.pointer as isize + arithmetic.advance) as usize;
    }

    fn fast(&mut self, operation: &FastOp) -> bool {
        match operation {
            FastOp::Search(search) => return search.execute(self),
            FastOp::Routed(routed) => return routed.execute(self),
            FastOp::Block { arithmetic, next } => {
                if !self.range_fits(arithmetic.minimum, arithmetic.maximum) {
                    return false;
                }
                self.apply(arithmetic, 1);
                self.pc = *next;
            }
            FastOp::Affine {
                arithmetic,
                inverse,
                next,
            } => {
                let value = self.tape[self.pointer];
                if value != 0 {
                    if !self.range_fits(arithmetic.minimum, arithmetic.maximum) {
                        return false;
                    }
                    let iterations = value.wrapping_neg().wrapping_mul(*inverse);
                    self.apply(arithmetic, iterations);
                    self.write_tape(self.pointer, 0);
                }
                self.pc = *next;
            }
            FastOp::Scan {
                stride,
                minimum,
                maximum,
                next,
            } => {
                return self.scan(*stride, *minimum, *maximum, *next);
            }
            FastOp::Walk { arithmetic, next } => {
                if let [(offset, delta)] = arithmetic.changes.as_slice() {
                    return if *offset == 0 {
                        self.walk_current(arithmetic, *delta, *next)
                    } else {
                        self.walk_offset(arithmetic, *offset, *delta, *next)
                    };
                }
                // Moving arithmetic loops can change their next predicate.
                // Execute complete bodies in order, including all writes;
                // only the per-iteration interpreter dispatch is removed.
                // Bound both the number of bodies and the total cell updates.
                for _ in 0..SCAN_LIMIT / (arithmetic.changes.len() + 1) {
                    if self.tape[self.pointer] == 0 {
                        self.pc = *next;
                        return true;
                    }
                    if !self.range_fits(arithmetic.minimum, arithmetic.maximum) {
                        return false;
                    }
                    self.apply(arithmetic, 1);
                }
                if self.tape[self.pointer] == 0 {
                    self.pc = *next;
                }
            }
        }
        true
    }

    fn walk_offset(
        &mut self,
        arithmetic: &Arithmetic,
        offset: isize,
        delta: u16,
        next: usize,
    ) -> bool {
        if self.tape[self.pointer] == 0 {
            self.pc = next;
            return true;
        }
        if !self.range_fits(arithmetic.minimum, arithmetic.maximum) {
            return false;
        }
        let step = arithmetic.advance.unsigned_abs();
        // Include cancelled pointer excursions, not just the write and final
        // pointer. Every complete body in this monotonic batch fits the tape.
        let safe_steps = if arithmetic.advance > 0 {
            (self.tape.len() - 1 - self.pointer - arithmetic.maximum as usize) / step + 1
        } else {
            (self.pointer - arithmetic.minimum.unsigned_abs()) / step + 1
        };
        let mut limit = safe_steps;
        let mut known_nonzero = false;
        if offset.unsigned_abs() % step != 0 {
            // The write and predicate use distinct residues. A cached run of
            // nonzero predicates cannot change during this batch, so its
            // interior needs no repeated reads. No layout or value is assumed.
            let (_, known) = self.scan_cache.prepare(step, self.pointer);
            if known.contains(self.pointer) {
                limit = limit.min(known.steps_from(self.pointer, arithmetic.advance));
                known_nonzero = true;
            }
        }
        let first = (self.pointer as isize + offset) as usize;
        if known_nonzero {
            // Distinct predicate and destination residues make the predicates
            // invariant. A proved constant destination run can therefore be
            // updated without visiting its interior, just like a cached scan.
            let mut uniform = self.tape.uniform_prefix(first, arithmetic.advance, limit);
            if let Some(zeros) = self.zero_cache.known_run(step, first) {
                let zero_count = limit.min(zeros.steps_from(first, arithmetic.advance));
                if uniform.is_none_or(|(count, _)| zero_count > count) {
                    uniform = Some((zero_count, 0));
                }
            }
            if let Some((count, old)) = uniform {
                let last = (first as isize + (count - 1) as isize * arithmetic.advance) as usize;
                let value = old.wrapping_add(delta);
                if self.tape.assign_uniform(first, last, step, old, value) {
                    if value == 0 && old != 0 {
                        self.scan_cache.invalidate_run(first, last, step);
                    } else if value != 0 && old == 0 {
                        self.zero_cache.invalidate_run(first, last, step);
                    }
                    if value == 0 {
                        let (slot, _) = self.zero_cache.prepare(step, first);
                        self.zero_cache.remember(slot, first, last);
                    } else {
                        let (slot, _) = self.scan_cache.prepare(step, first);
                        self.scan_cache.remember(slot, first, last);
                    }
                    self.pointer =
                        (self.pointer as isize + count as isize * arithmetic.advance) as usize;
                    if self.tape[self.pointer] == 0 {
                        self.pc = next;
                    }
                    return true;
                }
            }
        }
        limit = limit.min(SCAN_LIMIT / 2);
        let mut last = None;
        let mut created_zero = false;
        let mut created_nonzero = false;
        let mut all_zero = true;
        let mut all_nonzero = true;
        let mut uniform_value = None;
        let mut uniform = true;
        for _ in 0..limit {
            if !known_nonzero && self.tape[self.pointer] == 0 {
                break;
            }
            let target = (self.pointer as isize + offset) as usize;
            let old = self.tape[target];
            let value = old.wrapping_add(delta);
            self.tape[target] = value;
            created_zero |= value == 0 && old != 0;
            created_nonzero |= value != 0 && old == 0;
            all_zero &= value == 0;
            all_nonzero &= value != 0;
            if let Some(first_value) = uniform_value {
                uniform &= value == first_value;
            } else {
                uniform_value = Some(value);
            }
            last = Some(target);
            self.pointer = (self.pointer as isize + arithmetic.advance) as usize;
        }
        if let Some(last) = last {
            if uniform {
                let value = uniform_value.unwrap();
                // Every destination was committed by the point path above.
                // Retain a clean constant fact, even if unrelated strides
                // later evict the zero/nonzero predicate caches.
                self.tape.assign_uniform(first, last, step, value, value);
            }
            // Destinations are distinct on an in-bounds monotonic walk. Keep
            // every write while invalidating each cache only once per batch.
            if created_zero {
                self.scan_cache.invalidate_run(first, last, step);
            }
            if created_nonzero {
                self.zero_cache.invalidate_run(first, last, step);
            }
            if all_zero {
                let (slot, _) = self.zero_cache.prepare(step, first);
                self.zero_cache.remember(slot, first, last);
            } else if all_nonzero {
                let (slot, _) = self.scan_cache.prepare(step, first);
                self.scan_cache.remember(slot, first, last);
            }
        }
        if self.tape[self.pointer] == 0 {
            self.pc = next;
        }
        true
    }

    fn walk_current(&mut self, arithmetic: &Arithmetic, delta: u16, next: usize) -> bool {
        if let Some(known) = self
            .scan_cache
            .known_run(arithmetic.advance.unsigned_abs(), self.pointer)
        {
            if self.range_fits(arithmetic.minimum, arithmetic.maximum) {
                let step = arithmetic.advance.unsigned_abs();
                let safe_steps = if arithmetic.advance > 0 {
                    (self.tape.len() - 1 - self.pointer - arithmetic.maximum as usize) / step + 1
                } else {
                    (self.pointer - arithmetic.minimum.unsigned_abs()) / step + 1
                };
                let limit = safe_steps.min(known.steps_from(self.pointer, arithmetic.advance));
                let first = self.pointer;
                let uniform = self.tape.uniform_prefix(first, arithmetic.advance, limit);
                let mut count = (SCAN_LIMIT / 2).min(limit);
                let mut virtual_value = None;
                if let Some((uniform_count, old)) = uniform {
                    let last = (first as isize + (uniform_count - 1) as isize * arithmetic.advance)
                        as usize;
                    let value = old.wrapping_add(delta);
                    if self.tape.assign_uniform(first, last, step, old, value) {
                        count = uniform_count;
                        virtual_value = Some(value);
                    }
                }
                let last = (first as isize + (count - 1) as isize * arithmetic.advance) as usize;
                let mut any_zero = false;
                let mut all_zero = true;
                // A represented uniform run can be updated in constant work.
                // Unproved/heterogeneous cells retain bounded physical writes.
                if let Some(value) = virtual_value {
                    any_zero = value == 0;
                    all_zero = value == 0;
                } else {
                    let mut uniform_value = None;
                    let mut uniform = true;
                    for index in (first.min(last)..=first.max(last)).step_by(step) {
                        let value = self.tape[index].wrapping_add(delta);
                        self.tape[index] = value;
                        any_zero |= value == 0;
                        all_zero &= value == 0;
                        if let Some(first_value) = uniform_value {
                            uniform &= value == first_value;
                        } else {
                            uniform_value = Some(value);
                        }
                    }
                    if uniform {
                        let value = uniform_value.unwrap();
                        self.tape.assign_uniform(first, last, step, value, value);
                    }
                }
                self.pointer = (first as isize + count as isize * arithmetic.advance) as usize;
                if any_zero {
                    self.scan_cache.invalidate_run(first, last, step);
                }
                // Every previous value was nonzero: no zero-run fact became
                // false, even if some updated values remain nonzero.
                if all_zero {
                    let (slot, _) = self.zero_cache.prepare(step, first);
                    self.zero_cache.remember(slot, first, last);
                }
                if self.tape[self.pointer] == 0 {
                    self.pc = next;
                }
                return true;
            }
        }
        let first = self.pointer;
        let mut last = None;
        let mut created_zero = false;
        let mut all_zero = true;
        let mut success = true;
        let mut uniform_value = None;
        let mut uniform = true;
        // These bodies touch only their own predicate, and strict in-bounds
        // movement makes their cells distinct. Batch cache maintenance while
        // retaining every physical write and every intermediate bounds check.
        for _ in 0..SCAN_LIMIT / 2 {
            if self.tape[self.pointer] == 0 {
                break;
            }
            if !self.range_fits(arithmetic.minimum, arithmetic.maximum) {
                success = false;
                break;
            }
            let value = self.tape[self.pointer].wrapping_add(delta);
            self.tape[self.pointer] = value;
            created_zero |= value == 0;
            all_zero &= value == 0;
            if let Some(first_value) = uniform_value {
                uniform &= value == first_value;
            } else {
                uniform_value = Some(value);
            }
            last = Some(self.pointer);
            self.pointer = (self.pointer as isize + arithmetic.advance) as usize;
        }
        if let Some(last) = last {
            let step = arithmetic.advance.unsigned_abs();
            if uniform {
                let value = uniform_value.unwrap();
                self.tape.assign_uniform(first, last, step, value, value);
            }
            if created_zero {
                self.scan_cache.invalidate_run(first, last, step);
            }
            // All old predicates were nonzero, so zero-cache facts cannot have
            // been invalidated. A completely cleared run adds useful new facts.
            if all_zero {
                let (slot, _) = self.zero_cache.prepare(step, first);
                self.zero_cache.remember(slot, first, last);
            }
        }
        if self.tape[self.pointer] == 0 {
            self.pc = next;
        }
        success
    }

    /// Execute at most `budget` IR dispatches. Each optimized operation performs
    /// bounded work: basic summaries cover at most 128 arithmetic instructions,
    /// 256 scan/search probes, or 256 combined moving-loop
    /// iterations and cell updates. Routed loops inspect at most 128 parts per
    /// proof, with at most two proofs of 256 scan probes and bounded predicate
    /// checks; physical fill/maintenance work visits at most 1024 cells.
    /// Verified uniform progressions may be represented or restored in constant
    /// work, like cached scans. Every point/raw observation is logically exact.
    /// An input stop repeats until `provide_input` is called, without consuming
    /// another dispatch. Output and hooks have already advanced past the marker.
    pub fn run(&mut self, program: &Program, budget: u64) -> Result<Stop, Error> {
        self.run_impl::<false>(program, budget, None)
    }

    /// Execute with opt-in timings of generic BF operations. Timing calls and
    /// counters compile out of the ordinary `run` specialization.
    pub fn run_profiled(
        &mut self,
        program: &Program,
        budget: u64,
        profile: &mut Profile,
    ) -> Result<Stop, Error> {
        self.run_impl::<true>(program, budget, Some(profile))
    }

    fn run_impl<const PROFILE: bool>(
        &mut self,
        program: &Program,
        budget: u64,
        mut profile: Option<&mut Profile>,
    ) -> Result<Stop, Error> {
        if self.pending_input {
            return Ok(Stop::Input);
        }
        let mut remaining = budget;
        loop {
            if self.pc >= program.code.len() {
                return Ok(Stop::Halted);
            }
            if remaining == 0 {
                return Ok(Stop::Budget);
            }
            self.dispatches = self
                .dispatches
                .checked_add(1)
                .ok_or(Error::DispatchOverflow)?;
            remaining -= 1;
            if PROFILE {
                profile.as_deref_mut().unwrap().dispatch();
            }
            if self.tape.needs_materialization() {
                let started = PROFILE.then(std::time::Instant::now);
                self.tape.materialize_step();
                if PROFILE {
                    profile.as_deref_mut().unwrap().record(
                        profile::Kind::Materialize,
                        program.code[self.pc].offset,
                        started.unwrap().elapsed(),
                        true,
                    );
                }
                continue;
            }
            let fast = program.fast_at[self.pc];
            if fast != NO_FAST_OP {
                let operation = &program.fast[fast as usize];
                let offset = program.code[self.pc].offset;
                let started = PROFILE.then(std::time::Instant::now);
                let completed = self.fast(operation);
                if PROFILE {
                    profile.as_deref_mut().unwrap().record(
                        profile::Kind::fast(operation),
                        offset,
                        started.unwrap().elapsed(),
                        completed,
                    );
                }
                if completed {
                    continue;
                }
            }
            let instruction = program.code[self.pc];
            let started = PROFILE.then(std::time::Instant::now);
            let result = self.literal(instruction);
            if PROFILE {
                profile.as_deref_mut().unwrap().record(
                    profile::Kind::literal(instruction.op),
                    instruction.offset,
                    started.unwrap().elapsed(),
                    true,
                );
            }
            if let Some(stop) = result? {
                return Ok(stop);
            }
        }
    }

    #[inline]
    fn literal(&mut self, instruction: Instruction) -> Result<Option<Stop>, Error> {
        match instruction.op {
            Op::Add(delta) => {
                self.write_tape(self.pointer, self.tape[self.pointer].wrapping_add(delta));
                self.pc += 1;
            }
            Op::Move(delta) => {
                let next = self.pointer as i128 + delta as i128;
                if next >= 0 && next < self.tape.len() as i128 {
                    self.pointer = next as usize;
                } else if self.wrapping_tape {
                    self.pointer = next.rem_euclid(self.tape.len() as i128) as usize;
                } else {
                    // A collapsed monotonic move traps at the same edge as
                    // individual BF moves, after all preceding valid moves.
                    self.pointer = if delta < 0 { 0 } else { self.tape.len() - 1 };
                    return Err(Error::TapeBounds {
                        offset: instruction.offset,
                        pointer: self.pointer,
                    });
                }
                self.pc += 1;
            }
            Op::Open(next) => {
                self.pc = if self.tape[self.pointer] == 0 {
                    next
                } else {
                    self.pc + 1
                }
            }
            Op::Close(next) => {
                self.pc = if self.tape[self.pointer] != 0 {
                    next
                } else {
                    self.pc + 1
                }
            }
            Op::Input => {
                self.pc += 1;
                self.pending_input = true;
                return Ok(Some(Stop::Input));
            }
            Op::Output => {
                self.pc += 1;
                return Ok(Some(Stop::Output(self.tape[self.pointer] as u8)));
            }
            Op::Hook(marker) => {
                self.pc += 1;
                return Ok(Some(Stop::Hook(marker)));
            }
        }
        Ok(None)
    }
}

#[cfg(test)]
mod tests;

#[cfg(test)]
mod walk_tests;
