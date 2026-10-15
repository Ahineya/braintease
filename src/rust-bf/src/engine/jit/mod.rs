//! Opt-in native execution of hot generic Brainfuck regions.
//!
//! The immutable Program is borrowed for the lifetime of the native cache. Cells
//! remain wrapping u16, bounds are proved or guarded, and every I/O/host marker
//! returns through the ordinary interpreter. Complex loop summaries stay in Rust.
mod emit;
mod region;
mod stats;
pub use stats::JitStats;

use super::{Error, Machine, Program, Stop};
use region::{Footprint, Region};
use std::time::Instant;

const MAX_REGIONS: usize = 256;

struct NativeGate {
    slots: Vec<u16>,
}
impl NativeGate {
    const HOT: u16 = 32;
    const REJECTED: u16 = 33;
    const NATIVE: u16 = 34;
    #[inline]
    fn entry(&self, pc: usize) -> Option<usize> {
        let slot = self.slots[pc];
        (slot >= Self::NATIVE).then(|| usize::from(slot - Self::NATIVE))
    }
    #[inline]
    fn visit(&mut self, pc: usize) -> bool {
        let slot = &mut self.slots[pc];
        if *slot >= Self::NATIVE {
            return true;
        }
        if *slot < Self::HOT {
            *slot += 1;
            return *slot == Self::HOT;
        }
        false
    }
}

#[repr(C)]
struct State {
    pc: u64,
    pointer: u64,
    remaining: u64,
    low: u64,
    high: u64,
    length: u64,
}
type Native = unsafe extern "C" fn(*mut u16, *mut State);
struct Compiled {
    function: Native,
    footprint: Option<Footprint>,
    pcs: Vec<usize>,
    calls: u64,
    dispatches: u64,
}

pub struct Jit<'program> {
    program: &'program Program,
    compiler: emit::Compiler,
    regions: Vec<Compiled>,
    gate: NativeGate,
    stats: JitStats,
}

impl<'program> Jit<'program> {
    pub fn new(program: &'program Program) -> Result<Self, Error> {
        Ok(Self {
            program,
            compiler: emit::Compiler::new().map_err(Error::Jit)?,
            regions: Vec::new(),
            gate: NativeGate {
                slots: vec![0; program.code.len()],
            },
            stats: JitStats::default(),
        })
    }
    pub fn stats(&self) -> &JitStats {
        &self.stats
    }

    fn warm(&mut self, pc: usize) -> Result<(), Error> {
        if self.gate.entry(pc).is_some() || self.regions.len() >= MAX_REGIONS {
            return Ok(());
        }
        self.gate.slots[pc] = NativeGate::REJECTED;
        let Some(region) = Region::collect(self.program, pc, true) else {
            return Ok(());
        };
        // Straight stretches already have efficient Rust summaries. Native
        // execution needs a cycle to amortize its coherence and entry costs.
        if !region.has_cycle() {
            return Ok(());
        }
        self.install(pc, region)
    }

    fn install(&mut self, pc: usize, region: Region) -> Result<(), Error> {
        let footprint = region.footprint(pc);
        let started = Instant::now();
        let (function, bytes) = self
            .compiler
            .compile(&region, footprint.is_some())
            .map_err(Error::Jit)?;
        let elapsed = started.elapsed().as_secs_f64() * 1000.0;
        self.stats.compile_milliseconds += elapsed;
        self.stats.max_compile_milliseconds = self.stats.max_compile_milliseconds.max(elapsed);
        self.stats.compiled_operations += region.nodes.len();
        self.stats.native_bytes += bytes;
        let index = self.regions.len();
        for node in region.nodes.values() {
            if self.gate.entry(node.pc).is_none() {
                self.gate.slots[node.pc] = NativeGate::NATIVE + index as u16;
            }
        }
        self.regions.push(Compiled {
            function,
            footprint,
            pcs: region.nodes.keys().copied().collect(),
            calls: 0,
            dispatches: 0,
        });
        self.stats.compiled_regions = self.regions.len();
        Ok(())
    }

    /// Same synchronous events as Machine::run, with exact dispatch budgets.
    /// Compilation and tape preparation are bounded by fixed region limits.
    pub fn run(&mut self, machine: &mut Machine, budget: u64) -> Result<Stop, Error> {
        let mut remaining = budget;
        let mut window: Option<(usize, usize, *mut u16)> = None;
        let mut writes = [(0usize, 0u16); 128];
        loop {
            if machine.pending_input {
                return Ok(Stop::Input);
            }
            if machine.pc >= self.program.code.len() {
                return Ok(Stop::Halted);
            }
            if remaining == 0 {
                return Ok(Stop::Budget);
            }
            if !machine.tape.needs_materialization() {
                if self.gate.visit(machine.pc) && self.gate.entry(machine.pc).is_none() {
                    self.warm(machine.pc)?;
                }
                if let Some(index) = self.gate.entry(machine.pc) {
                    let compiled = &self.regions[index];
                    let available = remaining.min(u64::MAX - machine.dispatches);
                    if available != 0 {
                        let mut written_count = 0;
                        let (low, high, tape) = if let Some(footprint) = &compiled.footprint {
                            window = None;
                            let origin =
                                machine.pointer as i128 - footprint.offsets[&machine.pc] as i128;
                            let length = machine.tape_len();
                            if origin + (footprint.minimum as i128) < 0
                                || origin + footprint.maximum as i128 >= length as i128
                            {
                                self.stats.bounds_fallbacks += 1;
                                let before = machine.dispatches;
                                let stop = machine.run(self.program, 1)?;
                                remaining -= machine.dispatches - before;
                                if stop != Stop::Budget {
                                    return Ok(stop);
                                }
                                continue;
                            }
                            let mut tape = std::ptr::null_mut();
                            for (&offset, &written) in &footprint.cells {
                                let at = origin + offset as i128;
                                if at >= 0 && at < length as i128 {
                                    let at = at as usize;
                                    tape = machine.tape.prepare_native(at, at + 1);
                                    if written {
                                        writes[written_count] = (at, machine.tape[at]);
                                        written_count += 1;
                                    }
                                    if machine.tape.needs_materialization() {
                                        break;
                                    }
                                }
                            }
                            if machine.tape.needs_materialization() {
                                // Point splits are bounded; ordinary maintenance
                                // must reclaim metadata before a native call.
                                let before = machine.dispatches;
                                let stop = machine.run(self.program, 1)?;
                                remaining -= machine.dispatches - before;
                                if stop != Stop::Budget {
                                    return Ok(stop);
                                }
                                continue;
                            }
                            // A region with no valid memory cells can only move
                            // or fail a native bounds guard. Obtain the base
                            // without disturbing any unrelated logical cells.
                            if tape.is_null() {
                                tape = machine.tape.prepare_native(0, 1);
                            }
                            (0, length, tape)
                        } else if let Some(window) = window.filter(|(low, high, _)| {
                            *low <= machine.pointer && machine.pointer < *high
                        }) {
                            window
                        } else {
                            let low = machine
                                .pointer
                                .saturating_sub(512)
                                .min(machine.tape_len().saturating_sub(1024));
                            let high = (low + 1024).min(machine.tape_len());
                            let tape = machine.tape.prepare_native(low, high);
                            machine.scan_cache.invalidate_range(low, high - 1);
                            machine.zero_cache.invalidate_range(low, high - 1);
                            window = Some((low, high, tape));
                            (low, high, tape)
                        };
                        let mut state = State {
                            pc: machine.pc as u64,
                            pointer: machine.pointer as u64,
                            remaining: available,
                            low: low as u64,
                            high: high as u64,
                            length: machine.tape_len() as u64,
                        };
                        // SAFETY: a fixed graph has proved bounds and prepared
                        // every possible memory cell. Moving graphs guard each
                        // access against a prepared window and moves against the
                        // tape bounds. Native code cannot allocate or call Rust.
                        unsafe {
                            (compiled.function)(tape, &mut state);
                        }
                        for &(at, old) in &writes[..written_count] {
                            let new = machine.tape[at];
                            if old != 0 && new == 0 {
                                machine.scan_cache.invalidate(at);
                            }
                            if old == 0 && new != 0 {
                                machine.zero_cache.invalidate(at);
                            }
                        }
                        let used = available - state.remaining;
                        machine.pc = state.pc as usize;
                        machine.pointer = state.pointer as usize;
                        machine.dispatches += used;
                        remaining -= used;
                        self.stats.native_dispatches =
                            self.stats.native_dispatches.saturating_add(used);
                        self.stats.entries += 1;
                        let region = &mut self.regions[index];
                        region.calls = region.calls.saturating_add(1);
                        region.dispatches = region.dispatches.saturating_add(used);
                        if region.calls == 64 && region.dispatches < 64 * 16 {
                            for &pc in &region.pcs {
                                if self.gate.entry(pc) == Some(index) {
                                    self.gate.slots[pc] = NativeGate::REJECTED;
                                }
                            }
                            self.stats.retired_regions += 1;
                        }
                        if used != 0 {
                            continue;
                        }
                        self.stats.bounds_fallbacks += 1;
                        // A move may have left the old window on the preceding
                        // entry. Prepare once at the new pointer before falling back.
                        if !(low <= machine.pointer && machine.pointer < high) {
                            window = None;
                            continue;
                        }
                    }
                }
            }
            // Retain range-error partial state, wrapping aliases, deferred tape
            // maintenance and every existing guarded loop optimization.
            window = None;
            let before = machine.dispatches;
            // Sample ordinary execution in short batches rather than adding
            // heat/entry checks to every optimized interpreter dispatch. Long
            // native loops are likely to be sampled; short summaries stay Rust.
            let slice = if self.gate.entry(machine.pc).is_some() {
                1
            } else {
                remaining.min(if self.program.host_hooks_enabled {
                    64
                } else {
                    1
                })
            };
            let stop = machine.run(self.program, slice)?;
            remaining -= machine.dispatches - before;
            if stop != Stop::Budget {
                return Ok(stop);
            }
        }
    }
}

#[cfg(test)]
mod tests;
