//! Register Pressure Manager for V2 Backend

#![allow(dead_code)]
//! 
//! Based on the formalized register spilling algorithm with Sethi-Ullman
//! expression evaluation order to minimize register pressure.

use std::collections::{BTreeMap, BTreeSet, VecDeque};
use rcc_codegen::{AsmInst, Reg};
use rcc_common::TempId;
use rcc_ir::ir::{BasicBlock, Instruction, Value, IrBinaryOp};
use rcc_ir::BankTag;
use super::allocator::{RegAllocV2, ALLOCATABLE_REGISTERS};
use super::bank::BankInfo;
use log::{debug, trace};

/// Register need calculation result
#[derive(Debug, Clone)]
pub struct RegisterNeed {
    /// Number of registers needed to evaluate this expression
    pub count: usize,
    
    /// Is this a leaf node (constant, load, etc)
    pub is_leaf: bool,
}

/// Value lifetime information
#[derive(Debug, Clone)]
pub struct ValueLifetime {
    /// Instruction index where value is defined
    pub def_point: usize,
    
    /// Last instruction index where value is used
    pub last_use: Option<usize>,
    
    /// All instruction indices where value is used
    pub use_points: Vec<usize>,
    
    /// Whether this value crosses a function call
    pub crosses_call: bool,
}

/// LRU-based register allocator with Sethi-Ullman ordering
pub struct RegisterPressureManager {
    /// Base allocator for low-level operations (internal only)
    allocator: RegAllocV2,
    
    /// Free list of available registers
    free_list: VecDeque<Reg>,
    
    /// LRU queue of in-use registers (front = LRU, back = MRU)
    lru_queue: VecDeque<Reg>,
    
    /// Map from register to what it contains
    reg_contents: BTreeMap<Reg, String>,
    
    /// Map from register to spill slot (if spilled)
    reg_to_slot: BTreeMap<Reg, i16>,
    
    /// Map from value to spill slot (for reloading)
    value_to_slot: BTreeMap<String, i16>,

    /// A scalar register reloaded from its current private spill home is clean.
    /// This is a physical-register fact, discarded at CFG cache resets; the
    /// existence of a slot alone does not prove a store ran on every path.
    clean_spills: BTreeMap<Reg, (String, i16)>,

    /// Block-local resolved copies of immutable dynamic bank tags. The original
    /// tagged word remains the ABI value; only memory-address checks reuse this.
    resolved_banks: BTreeMap<String, String>,
    
    /// Next available spill slot
    next_spill_slot: i16,
    call_argument_slots: Vec<i16>,
    
    /// Lifetime information for values
    lifetimes: BTreeMap<TempId, ValueLifetime>,
    
    /// Instructions to emit
    instructions: Vec<AsmInst>,
    
    /// Number of local variables (for calculating spill addresses)
    local_count: i16,
    
    /// Map from alloca temp names to their FP offsets
    /// This allows recomputing alloca addresses when they're not in registers
    alloca_offsets: BTreeMap<String, i16>,
    
    /// Enable detailed spill/reload tracing
    trace_spills: bool,
    
    /// Map from fat pointer values to their bank spill slots
    /// When we spill a fat pointer, we need to track both address and bank
    fat_ptr_bank_slots: BTreeMap<String, i16>,

    /// Map from I32 low-word value names to their high-word companion names
    i32_high: BTreeMap<String, String>,

    /// Map from I64 low-word names to companion word names (w1, w2, w3)
    i64_words: BTreeMap<String, [String; 3]>,

    /// Temps that hold IEEE float bit patterns (F32 uses i32_high pairing, F64 uses i64_words)
    fp32: std::collections::BTreeSet<String>,
    fp64: std::collections::BTreeSet<String>,

    /// Registers that must not be chosen as spill victims (live across
    /// an in-progress instruction that still holds the Reg by value).
    /// Nested pin/unpin is refcounted so overlapping I64 words (e.g. shift
    /// amount in `a[0]`) stay pinned until every holder unpins.
    pinned_registers: std::collections::BTreeMap<Reg, u32>,

    /// Linker-relocatable base of this TU's GP data (None if the TU has no globals).
    gp_base_label: Option<String>,
    memory_bank_size: u16,
    segmented_stack: bool,
    max_outgoing_stack_cells: u32,

    /// IR roots needed after the instruction currently being lowered. Older
    /// standalone lowering tests may omit this; whole functions always set it.
    live_after: Option<BTreeSet<TempId>>,

    /// When the next emitted block has this block as its sole predecessor,
    /// keep its physical registers and save only values used on other edges.
    /// None retains the conservative flush used for joins and backedges.
    branch_spill_roots: Option<BTreeSet<TempId>>,
    prefer_caller_saved: bool,
}

impl RegisterPressureManager {
    pub fn new(local_count: i16) -> Self {
        // Initialize free_list with allocatable registers from centralized constant
        let mut free_list = VecDeque::new();
        for &reg in ALLOCATABLE_REGISTERS.iter() {
            free_list.push_back(reg);
        }
        
        Self {
            allocator: RegAllocV2::new(),
            free_list,
            lru_queue: VecDeque::new(),
            reg_contents: BTreeMap::new(),
            reg_to_slot: BTreeMap::new(),
            value_to_slot: BTreeMap::new(),
            clean_spills: BTreeMap::new(),
            resolved_banks: BTreeMap::new(),
            next_spill_slot: 0,
            call_argument_slots: Vec::new(),
            lifetimes: BTreeMap::new(),
            instructions: Vec::new(),
            local_count,
            alloca_offsets: BTreeMap::new(),
            trace_spills: false,
            fat_ptr_bank_slots: BTreeMap::new(),
            i32_high: BTreeMap::new(),
            i64_words: BTreeMap::new(),
            fp32: std::collections::BTreeSet::new(),
            fp64: std::collections::BTreeSet::new(),
            pinned_registers: std::collections::BTreeMap::new(),
            gp_base_label: None,
            memory_bank_size: 64000,
            segmented_stack: false,
            max_outgoing_stack_cells: 0,
            live_after: None,
            branch_spill_roots: None,
            prefer_caller_saved: false,
        }
    }
    
    /// Initialize the allocator (must be called before use)
    pub fn init(&mut self) {
        debug!("Initializing RegisterPressureManager with {} locals", self.local_count);
        self.allocator.init_stack_bank();
        self.allocator.set_spill_base(self.local_count);
        // Take any initialization instructions from the allocator
        self.instructions.extend(self.allocator.take_instructions());
        trace!("  R13 initialized, spill base set to FP+{}", self.local_count);
    }

    pub fn set_memory_bank_size(&mut self, bank_size: u16) {
        assert_ne!(bank_size, 0, "memory bank size must be nonzero");
        self.memory_bank_size = bank_size;
    }

    pub fn memory_bank_size(&self) -> u16 {
        self.memory_bank_size
    }

    pub fn set_segmented_stack(&mut self, enabled: bool) { self.segmented_stack = enabled; }
    pub fn segmented_stack(&self) -> bool { self.segmented_stack }

    pub fn reserve_outgoing_stack_cells(&mut self, cells: u32) {
        self.max_outgoing_stack_cells = self.max_outgoing_stack_cells.max(cells);
    }

    pub fn max_outgoing_stack_cells(&self) -> u32 { self.max_outgoing_stack_cells }

    /// Prevent `reg` from being reused as a spill victim until `unpin_register`.
    pub fn pin_register(&mut self, reg: Reg) {
        let n = self.pinned_registers.entry(reg).or_insert(0);
        *n += 1;
        trace!("Pinning register {reg:?} (count {n})");
    }

    /// Allow `reg` to be spilled again (after matching pins).
    pub fn unpin_register(&mut self, reg: Reg) {
        match self.pinned_registers.get_mut(&reg) {
            Some(n) if *n > 1 => {
                *n -= 1;
                trace!("Unpinning register {reg:?} (count {n})");
            }
            Some(_) => {
                self.pinned_registers.remove(&reg);
                trace!("Unpinning register {reg:?} (count 0)");
            }
            None => {
                trace!("Unpinning register {reg:?} (was not pinned)");
            }
        }
    }

    /// Prefer caller-saved registers in leaves, which need no saved-register
    /// frame until real pressure exhausts the eight temporary registers.
    pub(crate) fn prefer_caller_saved(&mut self) {
        self.prefer_caller_saved = true;
    }

    fn take_free_register(&mut self) -> Option<Reg> {
        // Freed temporaries are appended to the queue. Sorting only once would
        // eventually choose every S register in a low-pressure leaf anyway.
        if self.prefer_caller_saved {
            if let Some(index) = self.free_list.iter().position(|reg| {
                !self.pinned_registers.contains_key(reg)
                    && !matches!(reg, Reg::S0 | Reg::S1 | Reg::S2 | Reg::S3)
            }) { return self.free_list.remove(index); }
        }
        let index = self.free_list.iter()
            .position(|reg| !self.pinned_registers.contains_key(reg))?;
        self.free_list.remove(index)
    }

    /// Keep instruction operands pinned, but discard unpinned values which
    /// have no use after the current IR instruction (e.g. parked call args).
    pub(crate) fn retain_live_after(&mut self) {
        if let Some(live) = self.live_after.clone() {
            self.retain_live_registers(&live);
        }
    }

    /// Move a declared assembly clobber's current value to its spill home.
    /// Pinning the now-empty register excludes it from operand allocation.
    pub(crate) fn reserve_clobber(&mut self, register: Reg) {
        self.spill_register(register);
        self.forget_registers(&[register]);
        self.pin_register(register);
    }

    /// Take the LRU unpinned register as a spill victim.
    fn take_spill_victim(&mut self) -> Reg {
        let pos = self.lru_queue.iter().position(|r| {
            !self.pinned_registers.contains_key(r) && ALLOCATABLE_REGISTERS.contains(r)
        })
            .expect("No spillable registers (all pinned)!");
        self.lru_queue.remove(pos).unwrap()
    }
    
    /// Enable or disable spill tracing
    pub fn set_trace_spills(&mut self, enabled: bool) {
        self.trace_spills = enabled;
        if enabled {
            eprintln!("\n=== SPILL TRACE ENABLED ===");
            eprintln!("Function initialized with {} local slots", self.local_count);
            eprintln!("Spill slots will start at FP+{}", self.local_count);
        }
    }

    /// Set the relocatable GP base label for this translation unit.
    pub fn set_gp_base_label(&mut self, label: Option<String>) {
        self.gp_base_label = label;
    }

    /// Relocatable GP base label, if this TU emitted a `.data` block.
    pub fn gp_base_label(&self) -> Option<&str> {
        self.gp_base_label.as_deref()
    }
    
    /// Get the number of local slots this manager was initialized with
    pub fn local_count(&self) -> i16 {
        self.local_count
    }

    /// Highest FP-relative cell index touched by any spill: FP + local_count + (next_spill_slot-1).
    /// None when nothing has been spilled yet.
    pub fn max_spill_fp_offset(&self) -> Option<i16> {
        if self.next_spill_slot > 0 {
            Some(self.local_count + self.next_spill_slot - 1)
        } else {
            None
        }
    }

    /// Top of caller-owned frame (locals + all spill slots).
    /// Use FP + this value + 1 as the minimum SP before any CALL.
    pub fn frame_high_water(&self) -> i16 {
        self.local_count + self.next_spill_slot
    }

    /// Reusable private homes for large outgoing argument lists. Reserve them
    /// before materialization so later register spills cannot overlap them.
    pub(crate) fn call_argument_slots(&mut self, count: usize) -> Result<Vec<i16>, String> {
        while self.call_argument_slots.len() < count {
            let offset = self.local_count.checked_add(self.next_spill_slot)
                .ok_or("call argument frame exceeds signed FP offsets")?;
            offset.checked_add(1).ok_or("call argument frame exceeds signed FP offsets")?;
            self.next_spill_slot = self.next_spill_slot.checked_add(1)
                .ok_or("call argument spill space exhausted")?;
            self.call_argument_slots.push(offset);
        }
        Ok(self.call_argument_slots[..count].to_vec())
    }
    
    /// Initialize SB for stack operations (called automatically when needed)
    fn ensure_sb_initialized(&mut self) {
        if !self.allocator.sb_initialized {
            self.allocator.init_stack_bank();
            // The init_stack_bank generates instructions, take them
            self.instructions.extend(self.allocator.take_instructions());
        }
    }
    
    /// Set pointer bank information
    pub fn set_pointer_bank(&mut self, ptr_value: String, bank: BankInfo) {
        trace!("Setting bank info for '{ptr_value}': {bank:?}");
        self.allocator.set_pointer_bank(ptr_value, bank);
    }
    
    /// Get pointer bank information
    pub fn get_pointer_bank(&self, ptr_value: &str) -> Option<BankInfo> {
        self.allocator.pointer_banks.get(ptr_value).cloned()
    }

    pub(crate) fn cached_resolved_bank(&self, value: &str) -> Option<String> {
        self.resolved_banks.get(value).cloned()
    }

    pub(crate) fn cache_resolved_bank(&mut self, value: String, resolved: String) {
        self.resolved_banks.insert(value, resolved);
    }

    pub(crate) fn invalidate_resolved_banks(&mut self) {
        self.resolved_banks.clear();
    }

    /// Record that `lo_name`'s high 16 bits live in the named value `hi_name`
    pub fn set_i32_high(&mut self, lo_name: String, hi_name: String) {
        trace!("Setting I32 high for '{lo_name}': {hi_name}");
        self.i32_high.insert(lo_name, hi_name);
    }

    /// Companion high-word name for a 32-bit value, if one was bound
    pub fn get_i32_high(&self, lo_name: &str) -> Option<String> {
        self.i32_high.get(lo_name).cloned()
    }

    /// Record that `lo_name`'s extra 64-bit words live in `words` (w1, w2, w3)
    pub fn set_i64_words(&mut self, lo_name: String, words: [String; 3]) {
        trace!("Setting I64 words for '{lo_name}': {words:?}");
        self.i64_words.insert(lo_name, words);
    }

    /// Companion word names for a 64-bit value, if one was bound
    pub fn get_i64_words(&self, lo_name: &str) -> Option<[String; 3]> {
        self.i64_words.get(lo_name).cloned()
    }

    pub fn set_fp32(&mut self, lo_name: String) {
        self.fp32.insert(lo_name);
    }

    pub fn is_fp32(&self, lo_name: &str) -> bool {
        self.fp32.contains(lo_name)
    }

    pub fn set_fp64(&mut self, lo_name: String) {
        self.fp64.insert(lo_name);
    }

    pub fn is_fp64(&self, lo_name: &str) -> bool {
        self.fp64.contains(lo_name)
    }
    
    /// Get bank register for a pointer (internal use)
    pub fn get_bank_register(&mut self, ptr_value: &str) -> Reg {
        self.ensure_sb_initialized();
        self.allocator.get_bank_register(ptr_value)
    }
    
    /// Load parameter from stack
    pub fn load_parameter(&mut self, param_idx: usize) -> Reg {
        self.ensure_sb_initialized();
        let reg = self.allocator.load_parameter(param_idx);
        self.instructions.extend(self.allocator.take_instructions());
        reg
    }
    
    /// Check if SB is initialized (internal use)
    pub(super) fn is_sb_initialized(&self) -> bool {
        self.allocator.sb_initialized
    }
    
    /// Get the list of callee-saved registers that have been used
    /// These need to be saved in prologue and restored in epilogue
    pub(super) fn get_used_callee_saved(&self) -> Vec<Reg> {
        self.allocator.get_used_callee_saved()
    }
    
    /// Calculate register need for an expression (Sethi-Ullman)
    pub fn calculate_need(&self, value: &Value) -> RegisterNeed {
        let need = match value {
            Value::Constant(_) | Value::Global(_) | Value::Function(_) => {
                RegisterNeed { count: 1, is_leaf: true }
            }
            Value::Temp(id) => {
                // If already in register, need 0, else need 1
                if self.reg_contents.values().any(|v| v == &format!("t{id}")) {
                    RegisterNeed { count: 0, is_leaf: false }
                } else {
                    RegisterNeed { count: 1, is_leaf: true }
                }
            }
            Value::FatPtr(_) => {
                // Fat pointers need 2 registers
                RegisterNeed { count: 2, is_leaf: false }
            }
            Value::ConstantArray(_) | Value::ConstantAggregate(_) | Value::AddressOffset { .. } => {
                // Constant arrays are initialized separately, not loaded into registers
                RegisterNeed { count: 0, is_leaf: true }
            }
            Value::Undef => {
                RegisterNeed { count: 0, is_leaf: true }
            }
        };
        trace!("calculate_need({value:?}) = {need:?}");
        need
    }
    
    /// Calculate register need for a binary operation
    pub fn calculate_binary_need(&self, lhs: &Value, rhs: &Value) -> (RegisterNeed, bool) {
        let left_need = self.calculate_need(lhs);
        let right_need = self.calculate_need(rhs);
        
        // Should evaluate right first if it needs more registers
        let swap = right_need.count > left_need.count;
        
        let total_need = if left_need.count == right_need.count {
            // If both need same, we need one extra register
            RegisterNeed { 
                count: left_need.count + 1, 
                is_leaf: false 
            }
        } else {
            // We can reuse registers, need max of both
            RegisterNeed { 
                count: left_need.count.max(right_need.count), 
                is_leaf: false 
            }
        };
        
        trace!("calculate_binary_need: left={left_need:?}, right={right_need:?}, total={total_need:?}, swap={swap}");
        
        (total_need, swap)
    }
    
    /// Get a register using LRU spilling
    pub fn get_register(&mut self, for_value: String) -> Reg {
        trace!("get_register('{}'), LRU queue: {:?}", for_value, self.lru_queue);
        
        // Check if already in a register
        if let Some((&reg, _)) = self.reg_contents.iter().find(|(_, v)| *v == &for_value) {
            // Move to back of LRU (most recently used)
            if let Some(pos) = self.lru_queue.iter().position(|&r| r == reg) {
                self.lru_queue.remove(pos);
                self.lru_queue.push_back(reg);
                trace!("  '{for_value}' already in {reg:?}, moved to MRU position");
            }
            return reg;
        }
        
        // Check if this is an alloca that needs to be recomputed
        if let Some(&fp_offset) = self.alloca_offsets.get(&for_value) {
            debug!("  '{for_value}' is an alloca at FP+{fp_offset}, recomputing address");
            
            // Get a register for the recomputed address
            let reg = if let Some(free_reg) = self.take_free_register() {
                free_reg
            } else {
                // Need to spill to make room
                let victim = self.take_spill_victim();
                debug!("  Spilling {victim:?} to make room for alloca recomputation");
                self.spill_register(victim);
                victim
            };
            
            // Generate instructions to recompute FP + offset
            self.instructions.push(AsmInst::Comment(format!("Recompute alloca {for_value} at FP+{fp_offset}")));
            self.instructions.push(AsmInst::Add(reg, Reg::Fp, Reg::R0));
            if fp_offset != 0 {
                self.instructions.push(AsmInst::AddI(reg, reg, fp_offset));
            }
            
            // Update tracking
            self.reg_contents.insert(reg, for_value.clone());
            self.lru_queue.push_back(reg);
            
            debug!("  Recomputed alloca '{for_value}' into {reg:?}");
            return reg;
        }
        
        // Check if this value was previously spilled and needs reloading
        if let Some(&slot) = self.value_to_slot.get(&for_value) {
            debug!("  '{for_value}' was spilled to slot {slot}, reloading");
            
            // Get a register for the reload (might trigger another spill)
            let reg = if let Some(free_reg) = self.take_free_register() {
                free_reg
            } else {
                // Need to spill to make room
                let victim = self.take_spill_victim();
                debug!("  Spilling {victim:?} to make room for reload");
                self.spill_register(victim);
                victim
            };
            
            // Ensure R13 is initialized before any stack operation
            self.ensure_sb_initialized();
            
            // Generate reload instructions
            self.instructions.push(AsmInst::Comment(format!("Reload {for_value} from slot {slot}")));
            self.instructions.push(AsmInst::AddI(Reg::Sc, Reg::Fp, self.local_count + slot));
            self.instructions.push(AsmInst::Load(reg, Reg::Sb, Reg::Sc));
            self.clean_spills.insert(reg, (for_value.clone(), slot));
            
            // Check if this value has an associated bank that was spilled
            if self.fat_ptr_bank_slots.contains_key(&for_value) {
                if let Some(bank_info) = self.get_pointer_bank(&for_value) {
                    if let BankInfo::Dynamic(bank_name) = bank_info {
                        // Its named value owns the canonical home. A second
                        // pointer-private reload can read a slot initialized
                        // on only one predecessor and overwrite the good bank.
                        self.get_register(bank_name);
                    }
                }
            }
            
            // Update tracking
            self.reg_contents.insert(reg, for_value.clone());
            self.lru_queue.push_back(reg);
            
            debug!("  Reloaded '{for_value}' into {reg:?} from slot {slot}");
            return reg;
        }
        
        // Try to get a free register
        if let Some(reg) = self.take_free_register() {
            self.reg_contents.insert(reg, for_value.clone());
            self.lru_queue.push_back(reg);
            debug!("Allocated {reg:?} for '{for_value}' (was free)");
            trace!("  Free list now: {:?}", self.free_list);
            return reg;
        }
        
        // Need to spill - pick LRU unpinned victim
        debug!("No free registers, need to spill for '{for_value}'");
        let victim = self.take_spill_victim();
        
        debug!("Spilling LRU victim {victim:?} to make room");
        self.spill_register(victim);
        
        // Now victim is free
        self.reg_contents.insert(victim, for_value.clone());
        self.lru_queue.push_back(victim);
        debug!("Reused {victim:?} for '{for_value}' after spilling");
        victim
    }
    
    /// Spill a register to stack
    fn spill_register(&mut self, reg: Reg) {
        if let Some(value) = self.reg_contents.get(&reg).cloned() {
            trace!("spill_register({reg:?}) containing '{value}'");
            
            // Check if this is an alloca - allocas are addresses, don't spill them
            if self.alloca_offsets.contains_key(&value) {
                debug!("  '{value}' is an alloca address, not spilling (will recompute when needed)");
                if self.trace_spills {
                    eprintln!("SPILL: {} (alloca at FP+{}) from {:?} - NOT SPILLED (will recompute)",
                             value, self.alloca_offsets[&value], reg);
                }
                // Just remove from register, it will be recomputed when needed
                self.reg_contents.remove(&reg);
                return;
            }
            
            self.ensure_sb_initialized();
            
            // Get or allocate spill slot
            // IMPORTANT: Check if the VALUE already has a slot, not the register
            let slot = self.value_to_slot.get(&value).copied()
                .unwrap_or_else(|| {
                    let s = self.next_spill_slot;
                    self.next_spill_slot += 1;
                    self.reg_to_slot.insert(reg, s);
                    self.value_to_slot.insert(value.clone(), s);
                    trace!("  Allocated new spill slot {s} for '{value}'");
                    s
                });
            
            // Only scalar SSA values participate. Pointer banks and wide
            // companions keep the established coordinated spilling path.
            let clean = self.clean_spills.get(&reg) == Some(&(value.clone(), slot))
                && value.strip_prefix('t').is_some_and(|s| !s.is_empty() && s.bytes().all(|b| b.is_ascii_digit()))
                && self.get_pointer_bank(&value).is_none()
                && !self.i32_high.contains_key(&value) && !self.i64_words.contains_key(&value);
            if !clean {
                self.instructions.push(AsmInst::Comment(format!("Spill {value} to slot {slot}")));
                self.instructions.push(AsmInst::AddI(Reg::Sc, Reg::Fp, self.local_count + slot));
                self.instructions.push(AsmInst::Store(reg, Reg::Sb, Reg::Sc));
            }
            
            // Check if this value has associated bank info that needs spilling
            if let Some(bank_info) = self.get_pointer_bank(&value) {
                if let BankInfo::Dynamic(bank_name) = &bank_info {
                    debug!("  '{value}' has Dynamic bank '{bank_name}', spilling bank too");
                    
                    // Check if bank value is in a register
                    if let Some((&bank_reg, _)) = self.reg_contents.iter().find(|(_, v)| *v == bank_name) {
                        // Allocate a slot for the bank if not already allocated
                        let bank_slot = self.value_to_slot.get(bank_name).copied()
                            .unwrap_or_else(|| {
                                let s = self.next_spill_slot;
                                self.next_spill_slot += 1;
                                self.value_to_slot.insert(bank_name.clone(), s);
                                self.reg_to_slot.insert(bank_reg, s);
                                trace!("  Allocated bank spill slot {s} for '{value}'");
                                s
                            });
                        self.fat_ptr_bank_slots.insert(value.clone(), bank_slot);
                        
                        // Spill the bank register
                        self.instructions.push(AsmInst::Comment(format!("Spill {bank_name} to slot {bank_slot}")));
                        self.instructions.push(AsmInst::AddI(Reg::Sc, Reg::Fp, self.local_count + bank_slot));
                        self.instructions.push(AsmInst::Store(bank_reg, Reg::Sb, Reg::Sc));
                        
                        debug!("  Spilled bank '{}' from {:?} to slot {} (FP+{})", 
                               bank_name, bank_reg, bank_slot, self.local_count + bank_slot);
                    }
                }
            }
            
            debug!("Spilled '{}' from {:?} to slot {} (FP+{})", value, reg, slot, self.local_count + slot);

            // BankInfo::Register(this_reg) is now stale. The spilled contents live
            // under `value`; retarget those pointers so the bank can be reloaded.
            let stale: Vec<String> = self.allocator.pointer_banks.iter()
                .filter_map(|(ptr, info)| {
                    if matches!(info, BankInfo::Register(r) if *r == reg) {
                        Some(ptr.clone())
                    } else {
                        None
                    }
                })
                .collect();
            for ptr in stale {
                debug!("  Updating bank of '{ptr}' from Register({reg:?}) to Dynamic('{value}')");
                self.allocator.pointer_banks.insert(ptr, BankInfo::Dynamic(value.clone()));
            }
        } else {
            trace!("spill_register({reg:?}) - register was empty");
        }
        
        self.reg_contents.remove(&reg);
    }
    
    /// Reload a value from spill slot
    pub fn reload_value(&mut self, value: String) -> Reg {
        trace!("reload_value('{}'), value_to_slot: {:?}", value, self.value_to_slot);
        
        // Check if already in register
        if let Some((&reg, _)) = self.reg_contents.iter().find(|(_, v)| *v == &value) {
            trace!("  '{value}' already in {reg:?}, no reload needed");
            return reg;
        }
        
        // Check if spilled
        if let Some(&slot) = self.value_to_slot.get(&value) {
            debug!("Reloading '{value}' from spill slot {slot}");
            let reg = self.get_register(value.clone());
            
            // Ensure R13 is initialized before any stack operation
            self.ensure_sb_initialized();
            
            // Generate reload instructions
            self.instructions.push(AsmInst::Comment(format!("Reload {value} from slot {slot}")));
            self.instructions.push(AsmInst::AddI(Reg::Sc, Reg::Fp, self.local_count + slot));
            self.instructions.push(AsmInst::Load(reg, Reg::Sb, Reg::Sc));
            
            debug!("Reloaded '{}' into {:?} from slot {} (FP+{})", value, reg, slot, self.local_count + slot);
            return reg;
        }
        
        // Not spilled, allocate new
        trace!("  '{value}' not spilled, allocating new register");
        self.get_register(value)
    }
    
    /// Free a register
    pub fn free_register(&mut self, reg: Reg) {
        // Operand lowerers used to assume a temporary had only one use. SSA
        // promotion and LICM deliberately share values between instructions.
        if self.register_is_live_after(reg) { return; }
        self.clean_spills.remove(&reg);
        trace!("free_register({reg:?})");
        if let Some(value) = self.reg_contents.get(&reg) {
            debug!("Freeing {reg:?} containing '{value}'");
            self.resolved_banks.retain(|_, resolved| resolved != value);
        }
        if let Some(pos) = self.lru_queue.iter().position(|&r| r == reg) {
            self.lru_queue.remove(pos);
        }
        self.reg_contents.remove(&reg);
        if ALLOCATABLE_REGISTERS.contains(&reg) && !self.free_list.contains(&reg) {
            self.free_list.push_back(reg);
            trace!("  Added {reg:?} back to free list");
        }
    }

    /// Install the next instruction's liveness before any operand lowering.
    pub(crate) fn set_live_after(&mut self, live: &BTreeSet<TempId>) {
        self.live_after = Some(live.clone());
    }

    /// Whether overwriting or freeing this register would lose a later IR use,
    /// including a pointer bank or wide companion reachable through an alias.
    pub(crate) fn register_is_live_after(&self, register: Reg) -> bool {
        let (Some(live), Some(value)) = (&self.live_after, self.reg_contents.get(&register)) else {
            return false;
        };
        self.live_value_names(live, false).contains(value)
    }

    fn live_value_names(&self, live: &BTreeSet<TempId>, include_pinned: bool) -> BTreeSet<String> {
        let mut retained: BTreeSet<String> = live.iter().map(|id| format!("t{id}")).collect();
        if include_pinned {
            for register in self.pinned_registers.keys() {
                if let Some(value) = self.reg_contents.get(register) {
                    retained.insert(value.clone());
                }
            }
        }
        let mut pending: Vec<String> = retained.iter().cloned().collect();
        while let Some(value) = pending.pop() {
            let mut dependencies = Vec::new();
            if let Some(high) = self.i32_high.get(&value) {
                dependencies.push(high.clone());
            }
            if let Some(words) = self.i64_words.get(&value) {
                dependencies.extend(words.iter().cloned());
            }
            if let Some(resolved) = self.resolved_banks.get(&value) {
                dependencies.push(resolved.clone());
            }
            match self.allocator.pointer_banks.get(&value) {
                Some(BankInfo::Dynamic(name)) => dependencies.push(name.clone()),
                Some(BankInfo::Register(register)) => {
                    if let Some(name) = self.reg_contents.get(register) {
                        dependencies.push(name.clone());
                    }
                }
                _ => {}
            }
            for dependency in dependencies {
                if retained.insert(dependency.clone()) { pending.push(dependency); }
            }
        }
        retained
    }

    /// Reclaim dead IR values and helper registers between complete instructions.
    /// Live roots retain transitive companions and pointer banks. Keep spill slots
    /// and metadata for alternative CFG paths that have not been emitted yet.
    pub(crate) fn retain_live_registers(&mut self, live: &BTreeSet<TempId>) {
        let retained = self.live_value_names(live, true);
        let dead: Vec<Reg> = self.reg_contents.iter()
            .filter(|(register, value)| !retained.contains(*value)
                && !self.pinned_registers.contains_key(register))
            .map(|(register, _)| *register).collect();
        for register in dead { self.free_register(register); }
    }
    
    /// Bind a value to a specific register (e.g., after a function call)
    /// This is used when we know a value is in a specific register (like Rv0 after a call)
    pub fn bind_value_to_register(&mut self, value: String, reg: Reg) {
        debug!("Binding '{value}' to {reg:?}");
        self.clean_spills.remove(&reg);
        self.resolved_banks.retain(|source, resolved| source != &value && resolved != &value);

        // ABI registers (Rv/A/X/…) must not enter the LRU. I64 div parks the
        // quotient in Rv0/Rv1/X0/X1 because they are not allocatable; if a
        // call return is bound to Rv0 and later spilled, get_register reuses
        // Rv0 as a general register and those two uses collide.
        if !ALLOCATABLE_REGISTERS.contains(&reg) {
            if self.value_to_slot.remove(&value).is_some() {
                debug!("  Cleared existing spill slot for '{value}' to prevent stale reload");
            }
            if self.reg_contents.get(&reg).map(|v| v == &value).unwrap_or(false) {
                self.reg_contents.remove(&reg);
            }
            let dest = self.get_register(value);
            if dest != reg {
                self.instructions.push(AsmInst::Comment(format!(
                    "Copy {reg:?} to allocatable {dest:?}"
                )));
                self.instructions.push(AsmInst::Add(dest, reg, Reg::R0));
            }
            return;
        }
        
        // CRITICAL: Clear any existing spill slot for this value
        // This prevents reload of stale values when a value is recomputed
        if self.value_to_slot.remove(&value).is_some() {
            debug!("  Cleared existing spill slot for '{value}' to prevent stale reload");
        }
        
        // First, if the register contains something else, we need to handle that
        if let Some(old_value) = self.reg_contents.get(&reg).cloned() {
            if old_value != value {
                trace!("  {reg:?} previously contained '{old_value}', marking as free");
                // The old value is no longer in a register
            }
        }
        
        // Remove register from free list if it's there
        if let Some(pos) = self.free_list.iter().position(|&r| r == reg) {
            self.free_list.remove(pos);
            trace!("  Removed {reg:?} from free list");
        }
        
        // Update LRU queue
        if let Some(pos) = self.lru_queue.iter().position(|&r| r == reg) {
            self.lru_queue.remove(pos);
        }
        self.lru_queue.push_back(reg); // Mark as most recently used
        
        // Update contents
        self.reg_contents.insert(reg, value);
        trace!("  {reg:?} now contains bound value");
    }
    
    /// Clear any existing binding for a value
    /// This is used when we need to force recomputation of a value
    /// (e.g., GEP bank values in loops)
    pub fn clear_value_binding(&mut self, value: &str) {
        self.clean_spills.retain(|_, (name, _)| name != value);
        self.resolved_banks.retain(|source, resolved| source != value && resolved != value);
        self.instructions.push(AsmInst::Comment(format!("Clearing binding for '{value}'")));
        
        // Find if this value is in any register
        if let Some((&reg, _)) = self.reg_contents.iter().find(|(_, v)| v.as_str() == value) {
            debug!("Clearing binding for '{value}' from {reg:?}");
            self.instructions.push(AsmInst::Comment(format!("  Found in register {reg:?}, clearing")));
            self.reg_contents.remove(&reg);
            
            // Remove from LRU queue
            if let Some(pos) = self.lru_queue.iter().position(|&r| r == reg) {
                self.lru_queue.remove(pos);
            }
            
            // Add back to free list
            if ALLOCATABLE_REGISTERS.contains(&reg) && !self.free_list.contains(&reg) {
                self.free_list.push_back(reg);
            }
        }
        
        // ALSO clear from spill slots to prevent reload
        if self.value_to_slot.remove(value).is_some() {
            debug!("Also cleared '{value}' from spill slots");
            self.instructions.push(AsmInst::Comment("  Also cleared from spill slots".to_string()));
        }
    }
    
    /// Spill all unpinned registers (e.g., before a call).
    /// Pinned regs hold live call arguments and must stay put until they are
    /// copied to A0-A3 or stored to the outgoing argument area.
    pub fn spill_all(&mut self) {
        self.invalidate_resolved_banks();
        debug!("Spilling all unpinned registers (e.g., for function call)");
        trace!("  Current LRU queue: {:?}", self.lru_queue);
        trace!("  Current contents: {:?}", self.reg_contents);
        let regs_to_spill: Vec<Reg> = self.lru_queue.iter().copied()
            .filter(|r| !self.pinned_registers.contains_key(r))
            .collect();
        for reg in regs_to_spill {
            self.spill_register(reg);
            self.free_register(reg);
        }
        debug!("Unpinned registers spilled, {} slots used, {} still pinned",
               self.next_spill_slot, self.pinned_registers.len());
    }

    /// Flush the values needed on successor edges after materializing a branch
    /// condition. Spilling uses only SC as scratch and leaves every allocatable
    /// register's physical contents intact, so the branch can consume its
    /// captured condition even after its allocator binding is discarded.
    pub(crate) fn spill_live_for_branch(&mut self) {
        if let Some(live) = self.live_after.clone() {
            self.retain_live_registers(&live);
        }
        if let Some(roots) = &self.branch_spill_roots {
            let names = self.live_value_names(roots, false);
            let saved: Vec<_> = self.reg_contents.iter()
                .filter(|(_, value)| names.contains(*value))
                .map(|(&reg, value)| (reg, value.clone())).collect();
            for (reg, _) in &saved { self.spill_register(*reg); }
            // spill_register writes only SC and forgets its binding. Restore
            // tracking directly: bind_value_to_register would wrongly discard
            // the saved home needed by the alternative successor.
            for (reg, value) in saved { self.reg_contents.insert(reg, value); }
        } else {
            self.spill_all();
        }
    }

    pub(crate) fn set_branch_spill_roots(&mut self, roots: Option<BTreeSet<TempId>>) {
        // This is installed at every basic-block entry, including blocks whose
        // ordinary registers survive a uniquely entered fallthrough edge.
        self.invalidate_resolved_banks();
        self.branch_spill_roots = roots;
    }

    /// Save pinned call operands that remain live after the call. Their physical
    /// registers must stay reserved until argument stores / JALR consume them,
    /// but the callee may clobber them and their bindings are then forgotten.
    /// Do this before moving SP above the caller's final spill area.
    pub(crate) fn spill_live_pinned_for_call(&mut self) {
        let registers: Vec<Reg> = self.pinned_registers.keys().copied()
            .filter(|&register| self.register_is_live_after(register)).collect();
        for register in registers {
            self.spill_register(register);
        }
    }

    /// Drop tracking for registers that were used only to materialize stack
    /// arguments. After those values are stored to the outgoing arg area,
    /// the callee will clobber caller-saved T/S registers. If we keep the
    /// bindings (they were pinned during `spill_all`), the caller loads from
    /// a stale register instead of recomputing an alloca.
    ///
    /// Do not spill here: `SP` already points at the outgoing args, and a
    /// new spill slot can overwrite them.
    pub fn forget_all_registers(&mut self) {
        self.forget_registers(&ALLOCATABLE_REGISTERS);
    }

    pub fn forget_registers(&mut self, regs: &[Reg]) {
        self.invalidate_resolved_banks();
        for &reg in regs {
            self.clean_spills.remove(&reg);
            if !ALLOCATABLE_REGISTERS.contains(&reg) {
                continue;
            }
            debug!("Forgetting stack-arg register {reg:?} after outgoing store");
            if let Some(value) = self.reg_contents.remove(&reg) {
                trace!("  dropped binding '{value}' in {reg:?}");
            }
            if let Some(pos) = self.lru_queue.iter().position(|&r| r == reg) {
                self.lru_queue.remove(pos);
            }
            if !self.free_list.contains(&reg) {
                self.free_list.push_back(reg);
            }
        }
    }
    
    /// FP-relative offset of a registered alloca, if any.
    pub fn alloca_fp_offset(&self, name: &str) -> Option<i16> {
        self.alloca_offsets.get(name).copied()
    }

    /// Register an alloca temp with its FP offset
    pub fn register_alloca(&mut self, temp_name: String, fp_offset: i16) {
        debug!("Registering alloca '{temp_name}' at FP+{fp_offset}");
        self.alloca_offsets.insert(temp_name, fp_offset);
    }

    /// Definition emission may write directly into a previously allocated
    /// register; its old spill contents must no longer be treated as current.
    pub(crate) fn mark_ir_definition(&mut self, id: TempId) {
        let name = format!("t{id}");
        self.clean_spills.retain(|_, (value, _)| value != &name);
        self.resolved_banks.retain(|source, resolved| source != &name && resolved != &name);
    }

    pub(crate) fn invalidate_clean_spills(&mut self) {
        self.clean_spills.clear();
    }
    
    /// Get register for a Value
    pub fn get_value_register(&mut self, value: &Value) -> Reg {
        match value {
            Value::Temp(id) => {
                self.get_register(format!("t{id}"))
            }
            Value::Constant(val) => {
                let reg = self.get_register(format!("const_{val}"));
                self.instructions.push(AsmInst::Li(reg, *val as i16));
                reg
            }
            Value::Global(name) => {
                // For globals, we need to load the address
                let reg = self.get_register(format!("global_{name}"));
                // This would need proper global offset calculation
                self.instructions.push(AsmInst::Comment(format!("Load global {name}")));
                reg
            }
            Value::FatPtr(ptr) => {
                if matches!(ptr.bank, BankTag::Global) {
                    if let Value::Constant(c) = ptr.addr.as_ref() {
                        let reg = self.get_register(format!("gpoff_{c}"));
                        if let Some(label) = &self.gp_base_label {
                            self.instructions.push(AsmInst::LiLabel(reg, label.clone()));
                            if *c != 0 {
                                self.instructions.push(AsmInst::AddI(reg, reg, *c as i16));
                            }
                        } else {
                            self.instructions.push(AsmInst::Li(reg, *c as i16));
                        }
                        return reg;
                    }
                }
                self.get_value_register(&ptr.addr)
            }
            _ => {
                panic!("Unsupported value type for register allocation");
            }
        }
    }
    
    /// Emit a binary operation with Sethi-Ullman ordering
    pub fn emit_binary_op(&mut self, 
                          op: IrBinaryOp, 
                          lhs: &Value, 
                          rhs: &Value,
                          result_temp: TempId) -> Vec<AsmInst> {
        debug!("emit_binary_op({op:?}, lhs={lhs:?}, rhs={rhs:?}) -> t{result_temp}");
        let mut insts = Vec::new();
        
        // Calculate needs and determine evaluation order
        let (_, should_swap) = self.calculate_binary_need(lhs, rhs);
        
        let (first, second) = if should_swap {
            trace!("  Swapping operands for better register usage");
            (rhs, lhs)
        } else {
            (lhs, rhs)
        };
        
        // Evaluate in optimal order
        let first_reg = self.get_value_register(first);
        let second_reg = self.get_value_register(second);
        trace!("  Operands in {first_reg:?} and {second_reg:?}");
        
        // Emit the operation (reusing first register for result)
        let result_reg = first_reg;
        
        match op {
            IrBinaryOp::Add => {
                insts.push(AsmInst::Add(result_reg, first_reg, second_reg));
            }
            IrBinaryOp::Sub => {
                if should_swap {
                    // If we swapped, need to be careful with non-commutative ops
                    insts.push(AsmInst::Sub(result_reg, second_reg, first_reg));
                } else {
                    insts.push(AsmInst::Sub(result_reg, first_reg, second_reg));
                }
            }
            IrBinaryOp::Mul => {
                insts.push(AsmInst::Mul(result_reg, first_reg, second_reg));
            }
            IrBinaryOp::And => {
                insts.push(AsmInst::And(result_reg, first_reg, second_reg));
            }
            IrBinaryOp::Or => {
                insts.push(AsmInst::Or(result_reg, first_reg, second_reg));
            }
            IrBinaryOp::Xor => {
                insts.push(AsmInst::Xor(result_reg, first_reg, second_reg));
            }
            IrBinaryOp::Slt => {
                insts.push(AsmInst::Slt(result_reg, first_reg, second_reg));
            }
            // ... other operations
            _ => {
                insts.push(AsmInst::Comment(format!("TODO: Binary op {op:?}")));
            }
        }
        
        // Free the second register (first is reused for result)
        self.free_register(second_reg);
        
        // Update register contents to track the result
        self.reg_contents.insert(result_reg, format!("t{result_temp}"));
        
        insts
    }
    
    /// Analyze a basic block to build lifetime information
    pub fn analyze_block(&mut self, block: &BasicBlock) {
        for (idx, inst) in block.instructions.iter().enumerate() {
            match inst {
                Instruction::Binary { result, lhs, rhs, .. } => {
                    // Define result
                    self.lifetimes.entry(*result).or_insert(ValueLifetime {
                        def_point: idx,
                        last_use: None,
                        use_points: Vec::new(),
                        crosses_call: false,
                    });
                    
                    // Use operands
                    self.record_use(lhs, idx);
                    self.record_use(rhs, idx);
                }
                Instruction::Load { result, ptr, .. } => {
                    self.lifetimes.entry(*result).or_insert(ValueLifetime {
                        def_point: idx,
                        last_use: None,
                        use_points: Vec::new(),
                        crosses_call: false,
                    });
                    self.record_use(ptr, idx);
                }
                Instruction::Store { value, ptr, .. } => {
                    self.record_use(value, idx);
                    self.record_use(ptr, idx);
                }
                Instruction::Call { result, args, .. } => {
                    if let Some(res) = result {
                        self.lifetimes.entry(*res).or_insert(ValueLifetime {
                            def_point: idx,
                            last_use: None,
                            use_points: Vec::new(),
                            crosses_call: false,
                        });
                    }
                    
                    // Mark all live values as crossing a call
                    for lifetime in self.lifetimes.values_mut() {
                        if lifetime.def_point < idx {
                            if let Some(last) = lifetime.last_use {
                                if last > idx {
                                    lifetime.crosses_call = true;
                                }
                            }
                        }
                    }
                    
                    for arg in args {
                        self.record_use(arg, idx);
                    }
                }
                Instruction::Return(val) => {
                    if let Some(v) = val {
                        self.record_use(v, idx);
                    }
                }
                _ => {}
            }
        }
    }
    
    /// Record a use of a value
    fn record_use(&mut self, value: &Value, at_index: usize) {
        if let Value::Temp(id) = value {
            if let Some(lifetime) = self.lifetimes.get_mut(id) {
                lifetime.use_points.push(at_index);
                lifetime.last_use = Some(at_index);
            }
        }
    }
    
    /// Queue materialization alongside allocator spill/reload instructions.
    pub fn emit(&mut self, inst: AsmInst) { self.instructions.push(inst); }

    /// Take accumulated instructions
    pub fn take_instructions(&mut self) -> Vec<AsmInst> {
        std::mem::take(&mut self.instructions)
    }
    
    /// Get spill count for metrics
    pub fn get_spill_count(&self) -> usize {
        self.value_to_slot.len()
    }
    
    /// Invalidate alloca-register bindings
    /// This is called at basic block boundaries to ensure allocas are
    /// always recomputed fresh in loop headers, preventing stale register issues.
    pub fn invalidate_alloca_bindings(&mut self) {
        debug!("Invalidating alloca-register bindings at block boundary");
        
        // Find all registers that contain allocas
        let mut allocas_to_remove = Vec::new();
        for (reg, value) in self.reg_contents.iter() {
            if self.alloca_offsets.contains_key(value) {
                trace!("  Invalidating alloca '{value}' in {reg:?}");
                allocas_to_remove.push((*reg, value.clone()));
            }
        }
        
        // Count how many we're removing before consuming the vector
        let count = allocas_to_remove.len();
        
        // Remove allocas from registers and mark registers as free
        for (reg, _value) in allocas_to_remove {
            self.reg_contents.remove(&reg);
            // Remove from LRU queue
            if let Some(pos) = self.lru_queue.iter().position(|&r| r == reg) {
                self.lru_queue.remove(pos);
            }
            // Add back to free list if not already there
            if ALLOCATABLE_REGISTERS.contains(&reg) && !self.free_list.contains(&reg) {
                self.free_list.push_back(reg);
            }
        }
        
        debug!("  Invalidated {count} alloca bindings");
        if count > 0 {
            self.instructions.push(AsmInst::Comment(format!("Invalidated {count} alloca bindings")));
        }
    }
    
    /// Invalidate GEP bank register bindings
    /// This is called at basic block boundaries to ensure GEP-computed bank registers
    /// are reloaded from spill slots when needed in loops, preventing stale register issues.
    pub fn invalidate_gep_bank_bindings(&mut self) {
        debug!("Invalidating GEP bank register bindings at block boundary");
        
        // Find all registers that contain GEP bank values (Dynamics starting with "gep_")
        let mut gep_banks_to_remove = Vec::new();
        for (reg, value) in self.reg_contents.iter() {
            if value.starts_with("gep_new_bank_") || value.starts_with("gep_bank_") {
                trace!("  Invalidating GEP bank '{value}' in {reg:?}");
                gep_banks_to_remove.push((*reg, value.clone()));
            }
        }
        
        // Count how many we're removing before consuming the vector
        let count = gep_banks_to_remove.len();
        
        // Remove GEP banks from registers (but keep them in spill slots)
        for (reg, value) in gep_banks_to_remove {
            self.instructions.push(AsmInst::Comment(format!("Invalidating GEP bank {value} in {reg:?}")));
            self.reg_contents.remove(&reg);
            // Remove from LRU queue
            if let Some(pos) = self.lru_queue.iter().position(|&r| r == reg) {
                self.lru_queue.remove(pos);
            }
            // Add back to free list if not already there
            if !self.free_list.contains(&reg) {
                self.free_list.push_back(reg);
            }
        }
        
        debug!("  Invalidated {count} GEP bank bindings");
        if count > 0 {
            self.instructions.push(AsmInst::Comment(format!("Invalidated {count} GEP bank bindings")));
        }
    }
}

#[cfg(test)]
mod lifetime_reclamation_tests {
    use super::*;
    use crate::instr::tests::machine::Machine;

    #[test]
    fn pointer_bank_spill_home_does_not_depend_on_predecessor_spill_order() {
        let mut manager = RegisterPressureManager::new(0);
        manager.init();
        let pointer = manager.get_register("t1".into());
        let bank = manager.get_register("bank".into());
        manager.set_pointer_bank("t1".into(), BankInfo::Dynamic("bank".into()));
        // One predecessor spills the bank before the address.
        manager.spill_register(bank);
        manager.spill_register(pointer);
        let canonical_home = manager.value_to_slot["bank"];
        // Another spills the same live pair in the opposite order.
        let pointer = manager.get_register("t1".into());
        let bank = manager.get_register("bank".into());
        manager.spill_register(pointer);
        manager.spill_register(bank);
        assert_eq!(manager.fat_ptr_bank_slots["t1"], canonical_home);
        assert_eq!(manager.next_spill_slot, 2, "one home per word, independent of path");
        manager.forget_all_registers();
        manager.take_instructions();
        manager.get_register("t1".into());
        let reloads = manager.take_instructions();
        assert_eq!(reloads.iter().filter(|i| matches!(i, AsmInst::Load(..))).count(), 2,
            "reload each word once from its canonical home");
    }

    #[test]
    fn unchanged_scalar_reload_needs_no_second_store_but_redefinition_does() {
        let mut manager = RegisterPressureManager::new(0);
        manager.init();
        let reg = manager.get_register("t1".into());
        manager.emit(AsmInst::Li(reg, 1234));
        manager.spill_all();
        let mut instructions = manager.take_instructions();
        manager.get_register("t1".into());
        manager.spill_all();
        let unchanged = manager.take_instructions();
        assert!(unchanged.iter().any(|i| matches!(i, AsmInst::Load(..))));
        assert!(!unchanged.iter().any(|i| matches!(i, AsmInst::Store(..))));
        instructions.extend(unchanged);
        let reg = manager.get_register("t1".into());
        manager.emit(AsmInst::Li(reg, 4321));
        manager.mark_ir_definition(1);
        manager.spill_all();
        let changed = manager.take_instructions();
        assert!(changed.iter().any(|i| matches!(i, AsmInst::Store(..))));
        instructions.extend(changed);
        let result = manager.get_register("t1".into());
        manager.emit(AsmInst::Move(Reg::Rv0, result));
        instructions.extend(manager.take_instructions());
        let mut machine = Machine::new(64000);
        machine.regs[Reg::Fp as usize] = 100;
        machine.run(&instructions);
        assert_eq!(machine.regs[Reg::Rv0 as usize], 4321);
    }

    #[test]
    fn clean_spill_facts_do_not_cross_register_resets_or_memory_barriers() {
        let mut manager = RegisterPressureManager::new(0);
        manager.init();
        manager.get_register("t1".into());
        manager.spill_all();
        manager.get_register("t1".into());
        assert!(!manager.clean_spills.is_empty());
        manager.forget_all_registers();
        assert!(manager.clean_spills.is_empty());
        manager.get_register("t1".into());
        manager.invalidate_clean_spills();
        manager.take_instructions();
        manager.spill_all();
        assert!(manager.take_instructions().iter().any(|i| matches!(i, AsmInst::Store(..))));
    }

    #[test]
    fn leaf_preference_survives_repeated_register_reclamation() {
        let mut manager = RegisterPressureManager::new(0);
        manager.prefer_caller_saved();
        for index in 0..100 {
            let reg = manager.get_register(format!("helper{index}"));
            assert!(!matches!(reg, Reg::S0 | Reg::S1 | Reg::S2 | Reg::S3));
            manager.free_register(reg);
        }
        // Preference must still respect pinning and allow actual pressure to
        // use saved registers once every temporary register is occupied.
        let mut temps = Vec::new();
        for index in 0..8 {
            let reg = manager.get_register(format!("live{index}"));
            assert!(!matches!(reg, Reg::S0 | Reg::S1 | Reg::S2 | Reg::S3));
            manager.pin_register(reg);
            temps.push(reg);
        }
        let saved = manager.get_register("overflow".into());
        assert!(matches!(saved, Reg::S0 | Reg::S1 | Reg::S2 | Reg::S3));
        for reg in temps { manager.unpin_register(reg); }
    }

    #[test]
    fn spills_and_reloads_use_one_address_instruction_and_preserve_pointer_banks() {
        for locals in [0, 17, 32760] {
            let mut manager = RegisterPressureManager::new(locals);
            manager.init();
            let pointer = manager.get_register("t1".into());
            manager.emit(AsmInst::Li(pointer, 63999u16 as i16));
            let bank = manager.get_register("bank".into());
            manager.emit(AsmInst::Li(bank, 41));
            manager.set_pointer_bank("t1".into(), BankInfo::Dynamic("bank".into()));
            manager.spill_all();
            let pointer = manager.get_register("t1".into());
            manager.emit(AsmInst::Move(Reg::Rv0, pointer));
            let bank = manager.reload_value("bank".into());
            manager.emit(AsmInst::Move(Reg::Rv1, bank));
            let instructions = manager.take_instructions();
            let code: Vec<_> = instructions.iter().filter(|i| !matches!(i, AsmInst::Comment(_))).collect();
            let mut accesses = 0;
            for (index, inst) in code.iter().enumerate() {
                if matches!(inst, AsmInst::Load(_, Reg::Sb, Reg::Sc) | AsmInst::Store(_, Reg::Sb, Reg::Sc)) {
                    accesses += 1;
                    assert!(matches!(code[index - 1], AsmInst::AddI(Reg::Sc, Reg::Fp, _)));
                }
            }
            assert!(accesses >= 4);
            assert!(!code.iter().any(|inst| matches!(inst, AsmInst::Add(Reg::Sc, Reg::Fp, Reg::R0))));
            let mut machine = Machine::new(64000);
            machine.regs[Reg::Fp as usize] = 1000;
            machine.run(&instructions);
            assert_eq!(machine.regs[Reg::Rv0 as usize], 63999);
            assert_eq!(machine.regs[Reg::Rv1 as usize], 41);
            assert_eq!(machine.regs[Reg::Fp as usize], 1000);
        }
    }

    #[test]
    fn dead_helpers_do_not_spill_at_instruction_or_call_boundaries() {
        let mut manager = RegisterPressureManager::new(8);
        manager.init();
        let live = manager.get_register("t1".into());
        for instruction in 0..100 {
            for helper in 0..5 {
                manager.get_register(format!("helper_{instruction}_{helper}"));
            }
            manager.retain_live_registers(&BTreeSet::from([1]));
        }
        assert_eq!(manager.reg_contents, BTreeMap::from([(live, "t1".into())]));
        assert_eq!(manager.frame_high_water(), 8);
        assert!(!manager.take_instructions().iter().any(|i| matches!(i, AsmInst::Store(..))));
        manager.spill_all();
        assert_eq!(manager.get_spill_count(), 1);
        assert!(manager.value_to_slot.contains_key("t1"));
    }

    #[test]
    fn preserves_shared_wide_companions_pointer_banks_and_pinned_helpers() {
        let mut manager = RegisterPressureManager::new(0);
        manager.init();
        manager.get_register("t1".into());
        manager.get_register("shared_hi".into());
        manager.get_register("t2".into());
        for word in ["w1", "w2", "w3"] { manager.get_register(word.into()); }
        manager.get_register("t3".into());
        let bank = manager.get_register("pointer_bank".into());
        manager.set_i32_high("t1".into(), "shared_hi".into());
        // A dead source may share its high word with a live cast result.
        manager.set_i32_high("t99".into(), "shared_hi".into());
        manager.set_i64_words("t2".into(), ["w1".into(), "w2".into(), "w3".into()]);
        manager.set_pointer_bank("t3".into(), BankInfo::Register(bank));
        let pinned = manager.get_register("pinned_helper".into());
        manager.pin_register(pinned);
        manager.get_register("dead_helper".into());
        manager.retain_live_registers(&BTreeSet::from([1, 2, 3]));
        let values: BTreeSet<_> = manager.reg_contents.values().cloned().collect();
        assert_eq!(values, BTreeSet::from_iter([
            "t1", "shared_hi", "t2", "w1", "w2", "w3", "t3", "pointer_bank", "pinned_helper",
        ].into_iter().map(String::from)));
        manager.unpin_register(pinned);
        manager.retain_live_registers(&BTreeSet::new());
        assert!(manager.reg_contents.is_empty());
        assert!(manager.get_i32_high("t99").is_some());
    }

    #[test]
    fn preserves_spill_slots_for_later_emitted_alternative_path() {
        let mut manager = RegisterPressureManager::new(0);
        manager.init();
        manager.get_register("t1".into());
        manager.get_register("bank".into());
        manager.set_pointer_bank("t1".into(), BankInfo::Dynamic("bank".into()));
        manager.spill_all();
        let slots = manager.value_to_slot.clone();
        // First arm no longer uses the pointer. The other arm is emitted later.
        manager.get_register("t1".into());
        manager.retain_live_registers(&BTreeSet::new());
        assert!(manager.reg_contents.is_empty());
        assert_eq!(manager.value_to_slot, slots);
        manager.forget_all_registers();
        manager.take_instructions();
        manager.get_register("t1".into());
        assert!(manager.take_instructions().iter().any(|i| matches!(i, AsmInst::Load(..))));
        assert!(matches!(manager.get_pointer_bank("t1"), Some(BankInfo::Dynamic(name)) if name == "bank"));
    }
}
