//! Tiered, integer-only Cranelift execution of the unchanged Ripple ISA.
//!
//! Cold paths use the reference interpreter. Hot entries compile bounded regions
//! of real basic blocks; a jump table accepts entries at their leaders. A single
//! budget guard per basic block falls back to interpretation for a short batch
//! tail. PC/PCB and instruction counters are published at native exits and calls.
//!
//! Ordinary RAM is bounds checked in native code; bank-zero MMIO and faults use
//! the reference handlers. Header stores yield for IRQ and device processing.
//! STORC invalidates covering regions and changes an epoch that forces active
//! callers to unwind. Native calls have a depth cap independent of guest depth.
//! Invalidated machine code is reclaimed only after native frames have unwound.

mod emit;
mod host;
mod region;

use super::{Instr, VM};
use cranelift_codegen::ir::{types, AbiParam, UserFuncName};
use cranelift_jit::JITModule;
use cranelift_module::{FuncId, Linkage, Module};
use host::build_module;
use region::*;
use ripple_asm::Register;
use std::collections::HashMap;

const CONT: i32 = 0;
const YIELD: i32 = 1;
const FAULT: i32 = 2;
const FALLBACK: i32 = 3;

/// Limit both straight-line blocks and the number of leaders in a region.
/// Short batch tails interpret until the next published native entry.
const MAX_BLOCK: usize = 32;
const MAX_BLOCKS: usize = 64;
const REBUILD_AFTER: u32 = 32;
const HOT_THRESHOLD: u16 = 1024;

/// Cumulative costs for this VM's native execution, included in run reports.
#[derive(Clone, Debug, Default, serde::Serialize)]
pub struct JitStats {
    pub compiled_regions: u64,
    pub translated_instructions: u64,
    pub native_bytes: u64,
    pub compile_milliseconds: f64,
    pub max_compile_milliseconds: f64,
    pub interpreted_instructions: u64,
    pub native_instructions: u64,
}

type JitFn = unsafe extern "C" fn(*mut VM) -> i32;

#[derive(Clone, Copy)]
struct HostIds {
    load: FuncId,
    store: FuncId,
    loadc: FuncId,
    storc: FuncId,
    halt: FuncId,
    brk: FuncId,
    unknown: FuncId,
}

/// Cranelift's default memory provider deliberately retains executable pages on
/// drop. This VM owns every function pointer, so module destruction can reclaim
/// them once the driver has unwound the native call chain.
struct NativeModule(std::mem::ManuallyDrop<JITModule>);

impl NativeModule {
    fn new(module: JITModule) -> Self {
        Self(std::mem::ManuallyDrop::new(module))
    }
}

impl std::ops::Deref for NativeModule {
    type Target = JITModule;
    fn deref(&self) -> &JITModule {
        &self.0
    }
}

impl std::ops::DerefMut for NativeModule {
    fn deref_mut(&mut self) -> &mut JITModule {
        &mut self.0
    }
}

impl Drop for NativeModule {
    fn drop(&mut self) {
        // Engine/module replacement and VM destruction happen outside native
        // execution. No published pointer can be used after this drop.
        unsafe { std::mem::ManuallyDrop::take(&mut self.0).free_memory() };
    }
}

struct Compiled {
    /// Inclusive start and length of each basic block baked into this region.
    ranges: Vec<(usize, usize)>,
    physical: Vec<(usize, usize)>,
}

#[derive(Default)]
struct AddressCode {
    blocks: HashMap<usize, Compiled>,
    hits: Vec<u16>,
    covered: Vec<bool>,
    table: Vec<usize>,
    table_base: Box<usize>,
    code_limit: Option<usize>,
}
type CodeContext = (bool, bool, (u16, u16));
pub(super) struct JitEngine {
    context: CodeContext,
    contexts: HashMap<CodeContext, AddressCode>,
    module: NativeModule,
    ids: HostIds,
    blocks: HashMap<usize, Compiled>,
    dead: u32,
    seq: u32,
    hits: Vec<u16>,
    covered: Vec<bool>,
    stats: JitStats,
    code_limit: Option<usize>,
    /// Native function per guest instruction. Zero means "not compiled".
    /// `table_base` is a stable cell holding the allocation's address, which
    /// compiled calls reload so a later growth of `table` stays visible.
    table: Vec<usize>,
    table_base: Box<usize>,
}

/// Remove all entry points of a region together, never freeing native code
/// while a generated caller may still be unwinding through it.
fn discard(
    blocks: &mut HashMap<usize, Compiled>,
    table: &mut [usize],
    covered: &mut [bool],
    stale: &impl Fn(&Compiled) -> bool,
) -> bool {
    let mut removed = false;
    blocks.retain(|_, block| {
        if !stale(block) {
            return true;
        }
        removed = true;
        for &(start, len) in &block.ranges {
            table[start..start + len].fill(0);
            covered[start..start + len].fill(false);
        }
        false
    });
    removed
}
fn resolve_ranges(
    context: CodeContext,
    size: u16,
    memory: &[u16],
    limit: usize,
    ranges: &[(usize, usize)],
) -> Option<Vec<(usize, usize)>> {
    if !context.1 {
        return Some(ranges.to_vec());
    }
    let mut mmu = crate::mmu::Mmu::default();
    mmu.enabled = context.0;
    mmu.user = true;
    mmu.code_root = context.2;
    let mut physical: Vec<(usize, usize)> = Vec::new();
    for &(start, len) in ranges {
        for index in start..start + len {
            let (off, bank) = locate(size, index);
            let at = mmu.peek(memory, size, bank, off, crate::mmu::Access::Execute, limit)?;
            if let Some(last) = physical.last_mut() {
                if last.0 + last.1 == at {
                    last.1 += 1;
                    continue;
                }
            }
            physical.push((at, 1));
        }
    }
    Some(physical)
}

impl JitEngine {
    fn new() -> Result<Self, String> {
        let (module, ids) = build_module()?;
        Ok(Self {
            context: (false, false, (0, 0)),
            contexts: HashMap::new(),
            module: NativeModule::new(module),
            ids,
            blocks: HashMap::new(),
            dead: 0,
            seq: 0,
            hits: Vec::new(),
            covered: Vec::new(),
            stats: JitStats::default(),
            code_limit: None,
            table: Vec::new(),
            table_base: Box::new(0),
        })
    }

    fn select(&mut self, key: CodeContext) {
        if self.context == key {
            return;
        }
        let old = AddressCode {
            code_limit: self.code_limit.take(),
            blocks: std::mem::take(&mut self.blocks),
            hits: std::mem::take(&mut self.hits),
            covered: std::mem::take(&mut self.covered),
            table: std::mem::take(&mut self.table),
            table_base: std::mem::replace(&mut self.table_base, Box::new(0)),
        };
        self.contexts.insert(self.context, old);
        let new = self.contexts.remove(&key).unwrap_or_default();
        self.code_limit = new.code_limit;
        self.blocks = new.blocks;
        self.hits = new.hits;
        self.covered = new.covered;
        self.table = new.table;
        self.table_base = new.table_base;
        self.context = key;
    }

    fn ensure_table(&mut self, len: usize) {
        if len <= self.table.len() {
            return;
        }
        self.table.resize(len, 0);
        self.hits.resize(len, 0);
        self.covered.resize(len, false);
        *self.table_base = self.table.as_ptr() as usize;
    }

    fn publish(&mut self, index: usize, code: JitFn) {
        self.ensure_table(index + 1);
        self.table[index] = code as usize;
        *self.table_base = self.table.as_ptr() as usize;
    }

    fn get(&self, index: usize) -> Option<JitFn> {
        self.table
            .get(index)
            .copied()
            .filter(|&address| address != 0)
            .map(|address| unsafe { std::mem::transmute::<usize, JitFn>(address) })
    }

    fn hot(&mut self, index: usize, len: usize) -> bool {
        self.ensure_table(len);
        self.hits[index] = self.hits[index].saturating_add(1);
        self.hits[index] >= HOT_THRESHOLD
    }

    fn invalidate(&mut self, changed: std::ops::Range<usize>) {
        let overlaps = |block: &Compiled| {
            block
                .physical
                .iter()
                .any(|(start, len)| changed.start < start + len && *start < changed.end)
        };
        if discard(
            &mut self.blocks,
            &mut self.table,
            &mut self.covered,
            &overlaps,
        ) {
            self.dead += 1;
        }
        for code in self.contexts.values_mut() {
            if discard(
                &mut code.blocks,
                &mut code.table,
                &mut code.covered,
                &overlaps,
            ) {
                self.dead += 1;
            }
        }
    }

    fn sweep(&mut self) -> Result<(), String> {
        if self.dead >= REBUILD_AFTER {
            self.rebuild()?;
        }
        Ok(())
    }

    fn rebuild(&mut self) -> Result<(), String> {
        let (module, ids) = build_module()?;
        self.contexts.clear();
        self.blocks.clear();
        self.table.fill(0);
        self.covered.fill(false);
        self.module = NativeModule::new(module);
        self.ids = ids;
        self.dead = 0;
        log::debug!("jit reclaimed invalidated blocks");
        Ok(())
    }

    fn compile(
        &mut self,
        bank_size: u16,
        start: usize,
        region: &Region,
        code_len: usize,
        physical: Vec<(usize, usize)>,
    ) -> Result<JitFn, String> {
        let started = std::time::Instant::now();
        if bank_size == 0 {
            return Err("JIT: bank size is zero".into());
        }
        if region.blocks.is_empty() {
            return Err("JIT: empty region".into());
        }

        let ptr = self.module.target_config().pointer_type();
        let mut sig = self.module.make_signature();
        sig.params.push(AbiParam::new(ptr));
        sig.returns.push(AbiParam::new(types::I32));
        self.seq = self.seq.wrapping_add(1);
        let name = format!("ripple_block_{start}_{}", self.seq);
        let id = self
            .module
            .declare_function(&name, Linkage::Export, &sig)
            .map_err(|err| format!("JIT declare failed: {err}"))?;

        let mut ctx = self.module.make_context();
        ctx.func.signature = sig;
        ctx.func.name = UserFuncName::user(0, id.as_u32());
        self.translate(&mut ctx, bank_size, region, code_len)?;
        if let Err(err) = self.module.define_function(id, &mut ctx) {
            return Err(format!("JIT compile failed: {err}\n{}", ctx.func));
        }
        self.stats.native_bytes += ctx
            .compiled_code()
            .map_or(0, |code| code.code_buffer().len()) as u64;
        self.module.clear_context(&mut ctx);
        self.module
            .finalize_definitions()
            .map_err(|err| format!("JIT finalize failed: {err}"))?;
        let ptr = self.module.get_finalized_function(id);
        let code: JitFn = unsafe { std::mem::transmute(ptr) };
        log::debug!(
            "jit compiled region {start} ({} blocks)",
            region.blocks.len()
        );
        let milliseconds = started.elapsed().as_secs_f64() * 1000.0;
        self.stats.compiled_regions += 1;
        self.stats.translated_instructions += region
            .blocks
            .iter()
            .map(|b| b.instrs.len() as u64)
            .sum::<u64>();
        self.stats.compile_milliseconds += milliseconds;
        self.stats.max_compile_milliseconds = self.stats.max_compile_milliseconds.max(milliseconds);
        self.blocks.insert(
            start,
            Compiled {
                ranges: region.ranges(),
                physical,
            },
        );
        for block in &region.blocks {
            self.covered[block.index..block.index + block.instrs.len()].fill(true);
            self.publish(block.index, code);
        }
        Ok(code)
    }
}

impl VM {
    pub fn jit_stats(&self) -> Option<JitStats> {
        self.jit.as_ref().map(|engine| engine.stats.clone())
    }

    pub(crate) fn jit_flush(&mut self) {
        self.jit_epoch = self.jit_epoch.wrapping_add(1);
        if let Some(engine) = self.jit.as_mut() {
            engine.contexts.clear();
            engine.code_limit = None;
            engine.blocks.clear();
            engine.table.fill(0);
            engine.covered.fill(false);
            engine.hits.fill(0);
            engine.dead = engine.dead.saturating_add(1);
        }
    }

    /// Mapping flushes need not discard physical supervisor code or unchanged
    /// user mappings. Validate provenance for every decoded region, including
    /// inactive roots; physical code writes are invalidated separately.
    pub(crate) fn jit_flush_mappings(&mut self) {
        self.jit_epoch = self.jit_epoch.wrapping_add(1);
        let Some(engine) = self.jit.as_mut() else {
            return;
        };
        let size = self.bank_size;
        let memory = &self.memory;
        let limit = self.instructions.len();
        let context = engine.context;
        let stale = |block: &Compiled| {
            resolve_ranges(context, size, memory, limit, &block.ranges).as_ref()
                != Some(&block.physical)
        };
        if discard(
            &mut engine.blocks,
            &mut engine.table,
            &mut engine.covered,
            &stale,
        ) {
            engine.dead += 1;
        }
        engine.code_limit = None;
        for (context, code) in &mut engine.contexts {
            let stale = |block: &Compiled| {
                resolve_ranges(*context, size, memory, limit, &block.ranges).as_ref()
                    != Some(&block.physical)
            };
            if discard(&mut code.blocks, &mut code.table, &mut code.covered, &stale) {
                engine.dead += 1;
            }
            code.code_limit = None;
        }
    }

    pub(crate) fn jit_invalidate_index(&mut self, idx: usize) {
        self.jit_invalidate_range(idx..idx + 1);
    }

    pub(crate) fn jit_invalidate_range(&mut self, range: std::ops::Range<usize>) {
        self.jit_epoch = self.jit_epoch.wrapping_add(1);
        if let Some(engine) = self.jit.as_mut() {
            engine.invalidate(range);
        }
    }

    pub(super) fn run_batch_jit(&mut self, budget: u64) -> Result<u64, String> {
        if budget == 0 {
            return Ok(0);
        }
        match &self.state {
            super::VMState::Running => {}
            super::VMState::Setup => return Err("VM not initialized".into()),
            super::VMState::Error(error) => return Err(error.clone()),
            super::VMState::Halted | super::VMState::Breakpoint => return Ok(0),
        }
        if self.devices.has_pending_tick() || self.device_error.is_some() {
            return Ok(0);
        }
        self.jit_memory_base = self.memory.as_mut_ptr() as usize;
        self.jit_memory_len = self.memory.len() as u64;
        let before = self.executed_instructions;
        self.jit_budget = budget;
        self.jit_fault = None;
        self.mmu_pending = None;
        while self.jit_budget > 0 {
            if self.devices.has_pending_tick() || self.device_error.is_some() {
                break;
            }
            match &self.state {
                super::VMState::Running => {}
                super::VMState::Halted | super::VMState::Breakpoint => break,
                super::VMState::Error(error) => return Err(error.clone()),
                super::VMState::Setup => return Err("VM not initialized".into()),
            }
            self.maybe_dispatch_irq();
            let pc = self.registers[Register::Pc as usize];
            let pcb = self.registers[Register::Pcb as usize];
            let instr_idx = linear(self.bank_size, pc, pcb);
            // Fetch faults belong to the reference trap path, including counting.
            if self.get_current_instruction().is_none() {
                self.execute_one::<true>()?;
                self.jit_budget -= 1;
                return Ok(self.executed_instructions - before);
            }
            let Some(code) = self.jit_block(instr_idx)? else {
                // Cold paths stay on the reference interpreter. Count entries
                // at control-flow boundaries rather than compiling startup and
                // one-shot error handling merely because it was reached.
                for _ in 0..32.min(self.jit_budget) {
                    let inst = self.get_current_instruction();
                    let boundary = self.execute_one::<true>()?;
                    self.jit_budget -= 1;
                    self.jit.as_mut().unwrap().stats.interpreted_instructions += 1;
                    if boundary {
                        return Ok(self.executed_instructions - before);
                    }
                    let Some(inst) = inst else {
                        break;
                    };
                    if !is_plain(inst.opcode) || writes_pc(&inst) {
                        break;
                    }
                }
                continue;
            };
            let vm = self as *mut VM;
            let native_before = self.executed_instructions;
            let status = unsafe { code(vm) };
            self.jit.as_mut().unwrap().stats.native_instructions +=
                self.executed_instructions - native_before;
            if let Some(engine) = self.jit.as_mut() {
                engine.sweep()?;
            }
            if let Some(fault) = self.mmu_pending.take() {
                self.jit_fault = None;
                self.executed_instructions += 1;
                self.jit_budget -= 1;
                self.exception(fault, false)?;
                break;
            }
            if let Some(error) = self.jit_fault.take() {
                return Err(error);
            }
            match status {
                CONT => {}
                YIELD => break,
                FALLBACK => {
                    if self.jit_budget == 0 {
                        break;
                    }
                    let boundary = self.execute_one::<true>()?;
                    self.jit_budget -= 1;
                    self.jit.as_mut().unwrap().stats.interpreted_instructions += 1;
                    if boundary {
                        break;
                    }
                }
                FAULT => return Err("JIT fault".into()),
                other => return Err(format!("JIT status {other}")),
            }
        }
        Ok(self.executed_instructions - before)
    }

    fn jit_block(&mut self, index: usize) -> Result<Option<JitFn>, String> {
        if self.bank_size == 0 {
            return Err("JIT: bank size is zero".into());
        }
        if self.jit.is_none() {
            self.jit = Some(JitEngine::new()?);
        }
        let key = (
            self.mmu.enabled,
            self.mmu.user,
            if self.mmu.user {
                self.mmu.code_root
            } else {
                (0, 0)
            },
        );
        self.jit.as_mut().unwrap().select(key);
        let code_len = if self.mmu.user {
            if let Some(limit) = self.jit.as_ref().unwrap().code_limit {
                limit
            } else {
                let limit =
                    self.mmu
                        .code_limit(&self.memory, self.bank_size, self.code_memory_capacity());
                self.jit.as_mut().unwrap().code_limit = Some(limit);
                limit
            }
        } else {
            self.instructions.len()
        };
        if index >= code_len || self.registers[Register::Pc as usize] >= self.bank_size {
            return Ok(None);
        }
        if let Some(code) = self.jit.as_ref().and_then(|e| e.get(index)) {
            return Ok(Some(code));
        }
        let engine = self.jit.as_mut().unwrap();
        if engine.covered.get(index).copied().unwrap_or(false) || !engine.hot(index, code_len) {
            return Ok(None);
        }
        let region = if !self.mmu.user {
            build_region(self.bank_size, index, &self.instructions, &engine.covered)?
        } else {
            let size = self.bank_size;
            let memory = &self.memory;
            let code = &self.instructions;
            let mmu = &mut self.mmu;
            build_region_with(
                size,
                index,
                code_len,
                &mut |i| {
                    let (pc, pcb) = locate(size, i);
                    mmu.translate(
                        memory,
                        size,
                        pcb,
                        pc,
                        crate::mmu::Access::Execute,
                        code.len(),
                    )
                    .ok()
                    .and_then(|p| code.get(p).copied())
                    .unwrap_or(Instr::new(0xFF, 0xFF, 0, 0, 0))
                },
                &engine.covered,
            )?
        };
        let physical = resolve_ranges(
            engine.context,
            self.bank_size,
            &self.memory,
            self.instructions.len(),
            &region.ranges(),
        )
        .ok_or("JIT: instruction mapping changed during compilation")?;
        engine
            .compile(self.bank_size, index, &region, code_len, physical)
            .map(Some)
    }
}

#[cfg(test)]
mod tests;
