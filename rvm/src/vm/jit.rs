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
}

pub(super) struct JitEngine {
    module: NativeModule,
    ids: HostIds,
    blocks: HashMap<usize, Compiled>,
    dead: u32,
    seq: u32,
    hits: Vec<u16>,
    covered: Vec<bool>,
    stats: JitStats,
    /// Native function per guest instruction. Zero means "not compiled".
    /// `table_base` is a stable cell holding the allocation's address, which
    /// compiled calls reload so a later growth of `table` stays visible.
    table: Vec<usize>,
    table_base: Box<usize>,
}

impl JitEngine {
    fn new() -> Result<Self, String> {
        let (module, ids) = build_module()?;
        Ok(Self {
            module: NativeModule::new(module),
            ids,
            blocks: HashMap::new(),
            dead: 0,
            seq: 0,
            hits: Vec::new(),
            covered: Vec::new(),
            stats: JitStats::default(),
            table: Vec::new(),
            table_base: Box::new(0),
        })
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

    fn clear_slot(&mut self, index: usize) {
        if index < self.table.len() {
            self.table[index] = 0;
            self.covered[index] = false;
        }
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

    fn invalidate(&mut self, idx: usize) {
        let doomed: Vec<usize> = self
            .blocks
            .iter()
            .filter(|(_, block)| {
                block
                    .ranges
                    .iter()
                    .any(|(start, len)| idx >= *start && idx < start + len)
            })
            .map(|(key, _)| *key)
            .collect();
        if doomed.is_empty() {
            return;
        }
        let mut cover = Vec::new();
        for key in &doomed {
            if let Some(block) = self.blocks.get(key) {
                for &(start, len) in &block.ranges {
                    cover.extend(start..start + len);
                }
            }
            cover.push(*key);
        }
        for key in doomed {
            self.blocks.remove(&key);
        }
        for index in cover {
            self.clear_slot(index);
        }
        // One edit invalidates the whole region. Rebuilding the module is the
        // expensive path, and self-modifying stores are rare.
        self.dead = self.dead.saturating_add(1);
    }

    fn sweep(&mut self) -> Result<(), String> {
        if self.dead >= REBUILD_AFTER {
            self.rebuild()?;
        }
        Ok(())
    }

    fn rebuild(&mut self) -> Result<(), String> {
        let (module, ids) = build_module()?;
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
            engine.blocks.clear();
            engine.table.fill(0);
            engine.covered.fill(false);
            engine.hits.fill(0);
            engine.dead = engine.dead.saturating_add(1);
        }
    }

    pub(crate) fn jit_invalidate_index(&mut self, idx: usize) {
        self.jit_epoch = self.jit_epoch.wrapping_add(1);
        if let Some(engine) = self.jit.as_mut() {
            engine.invalidate(idx);
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
            let instr_idx = (pcb as usize * self.bank_size as usize) + pc as usize;
            if instr_idx >= self.instructions.len() {
                self.state = super::VMState::Error(format!(
                    "PC out of bounds: bank={pcb}, offset={pc}, idx={instr_idx}, total_instructions={}",
                    self.instructions.len()
                ));
                return Err(format!(
                    "PC out of bounds: idx={instr_idx} >= len={}",
                    self.instructions.len()
                ));
            }
            let Some(code) = self.jit_block(instr_idx)? else {
                // Cold paths stay on the reference interpreter. Count entries
                // at control-flow boundaries rather than compiling startup and
                // one-shot error handling merely because it was reached.
                for _ in 0..32.min(self.jit_budget) {
                    let inst = self.get_current_instruction();
                    let boundary = self.execute_one::<true>()?;
                    let inst = inst.expect("successful instruction fetch");
                    self.jit_budget -= 1;
                    self.jit.as_mut().unwrap().stats.interpreted_instructions += 1;
                    if boundary {
                        return Ok(self.executed_instructions - before);
                    }
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
        if self.registers[Register::Pc as usize] < self.bank_size {
            if let Some(code) = self.jit.as_ref().and_then(|engine| engine.get(index)) {
                return Ok(Some(code));
            }
        }
        if self.jit.is_none() {
            self.jit = Some(JitEngine::new()?);
        }
        if self.registers[Register::Pc as usize] >= self.bank_size
            || self
                .jit
                .as_ref()
                .unwrap()
                .covered
                .get(index)
                .copied()
                .unwrap_or(false)
            || !self
                .jit
                .as_mut()
                .unwrap()
                .hot(index, self.instructions.len())
        {
            return Ok(None);
        }
        let region = build_region(
            self.bank_size,
            index,
            &self.instructions,
            &self.jit.as_ref().unwrap().covered,
        )?;
        self.jit
            .as_mut()
            .ok_or_else(|| "JIT engine missing".to_string())?
            .compile(self.bank_size, index, &region, self.instructions.len())
            .map(Some)
    }
}

#[cfg(test)]
mod tests;
