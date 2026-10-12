//! Resumable, optimized Brainfuck execution with wrapping 16-bit cells.
//!
//! This module has no device dependencies. Input, output and optional host
//! markers are synchronous barriers: execution resumes only after the caller
//! has had an opportunity to inspect or modify the tape. Budgets and
//! `dispatches()` count IR dispatches, not original Brainfuck characters.

use std::collections::BTreeMap;
use std::fmt;

mod routed;
mod scan;
mod search;

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
    tape: Vec<u16>,
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
            tape,
            pointer: 0,
            pc: 0,
            dispatches: 0,
            pending_input: false,
            wrapping_tape,
            scan_cache: scan::Cache::default(),
            zero_cache: scan::Cache::default(),
        })
    }

    pub fn tape(&self) -> &[u16] {
        &self.tape
    }
    pub fn tape_mut(&mut self) -> &mut [u16] {
        // The caller can change any cell, including a cached scan destination.
        self.scan_cache.reset();
        self.zero_cache.reset();
        &mut self.tape
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
    #[inline]
    pub fn write_cell(&mut self, index: usize, value: u16) {
        // Both predicates depend only on zero-ness, not a particular value.
        let old = self.tape[index];
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
            self.write_cell(
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
                    self.write_cell(self.pointer, 0);
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
                if let [(0, delta)] = arithmetic.changes.as_slice() {
                    return self.walk_current(arithmetic, *delta, *next);
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

    fn walk_current(&mut self, arithmetic: &Arithmetic, delta: u16, next: usize) -> bool {
        let first = self.pointer;
        let mut last = None;
        let mut created_zero = false;
        let mut all_zero = true;
        let mut success = true;
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
            last = Some(self.pointer);
            self.pointer = (self.pointer as isize + arithmetic.advance) as usize;
        }
        if let Some(last) = last {
            let step = arithmetic.advance.unsigned_abs();
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
    /// checks; a counted fill examines and writes at most 1024 markers. A scan
    /// probe can skip a previously verified run of equal zero-ness in constant time.
    /// An input stop repeats until `provide_input` is called, without consuming
    /// another dispatch. Output and hooks have already advanced past the marker.
    pub fn run(&mut self, program: &Program, budget: u64) -> Result<Stop, Error> {
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
            let fast = program.fast_at[self.pc];
            if fast != NO_FAST_OP && self.fast(&program.fast[fast as usize]) {
                continue;
            }
            let instruction = program.code[self.pc];
            match instruction.op {
                Op::Add(delta) => {
                    self.write_cell(self.pointer, self.tape[self.pointer].wrapping_add(delta));
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
                    return Ok(Stop::Input);
                }
                Op::Output => {
                    self.pc += 1;
                    return Ok(Stop::Output(self.tape[self.pointer] as u8));
                }
                Op::Hook(marker) => {
                    self.pc += 1;
                    return Ok(Stop::Hook(marker));
                }
            }
        }
    }
}

#[cfg(test)]
mod tests;

#[cfg(test)]
mod walk_tests;
