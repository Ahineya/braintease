//! Global variable handling for V2 backend
//! 
//! This module manages global variable allocation, tracking, and initialization
//! code generation for the V2 backend.

use rcc_ir::ir::{GlobalVariable, IrType, Value, Linkage};
use rcc_codegen::{AsmInst, Reg, emit_addr_constant};
use std::collections::HashMap;
use log::debug;

/// Information about a global variable's allocation
#[derive(Debug, Clone)]
pub struct GlobalInfo {
    /// Linear cell offset in this translation unit, before link-time placement.
    pub address: u32,
    /// Size in 16-bit words
    pub size: u32,
}

/// Global variable manager for tracking and lowering globals
pub struct GlobalManager {
    /// Map from global name to allocation info
    allocations: HashMap<String, GlobalInfo>,
    /// Next available global address
    next_address: u32,
    /// Sanitized module name used to uniquify the GP base label
    module_name: String,
    definitions: Vec<(String, u32)>,
    bank_size: u16,
    banked_globals: bool,
}

fn sanitize_module_name(name: &str) -> String {
    let s: String = name.chars()
        .map(|c| if c.is_ascii_alphanumeric() { c } else { '_' })
        .collect();
    if s.is_empty() {
        "module".to_string()
    } else {
        s
    }
}

impl GlobalManager {
    /// Create a new global manager
    pub fn new() -> Self {
        Self::with_module("module")
    }

    /// Create a manager whose GP base label is unique to this translation unit
    pub fn with_module(module_name: &str) -> Self {
        Self::with_module_and_bank_size(module_name, 64000)
    }

    pub fn with_module_and_bank_size(module_name: &str, bank_size: u16) -> Self {
        assert_ne!(bank_size, 0, "bank size must be nonzero");
        Self {
            allocations: HashMap::new(),
            next_address: 0,
            module_name: format!("{}_{}", sanitize_module_name(module_name), uuid::Uuid::new_v4().simple()),
            definitions: Vec::new(),
            bank_size,
            banked_globals: false,
        }
    }

    /// Linker-relocatable label for word 0 of this TU's GP data, if any globals exist
    pub fn with_banked_globals(mut self, enabled: bool) -> Self {
        self.banked_globals = enabled;
        self
    }

    pub fn banked_globals(&self) -> bool { self.banked_globals }

    pub fn gp_base_label(&self) -> Option<String> {
        if self.next_address == 0 {
            None
        } else {
            Some(format!("__gp_{}", self.module_name))
        }
    }

    /// Total GP words allocated in this translation unit
    pub fn total_words(&self) -> u32 {
        self.next_address
    }

    /// `.data` reservation so the linker can concatenate GP ranges across objects
    pub fn emit_data_section(&self) -> Vec<AsmInst> {
        let Some(label) = self.gp_base_label() else {
            return Vec::new();
        };
        let mut insts = vec![AsmInst::Raw(".data".to_string()), AsmInst::Label(label)];
        for (name, size) in &self.definitions {
            insts.push(AsmInst::Label(name.clone()));
            insts.push(AsmInst::Raw(format!(".space {size}")));
        }
        insts.push(AsmInst::Raw(".code".to_string()));
        insts
    }

    pub fn unit_tag(&self) -> &str { &self.module_name }

    fn emit_gp_addr(&self, dest: Reg, address: u32) -> Vec<AsmInst> {
        let label = self.gp_base_label();
        emit_addr_constant(dest, address as i16, true, label.as_deref())
    }
    
    /// Initializer destinations retain their owning symbol so the linker may
    /// compact other globals without changing offsets within this object.
    fn emit_initializer_addr(&self, dest: Reg, global: &GlobalVariable, address: u32) -> Vec<AsmInst> {
        if let Some(info) = self.get_global_info(&global.name) {
            if self.banked_globals {
                return self.emit_banked_address(dest, Reg::T6, &global.name, i64::from(address) - i64::from(info.address));
            }
            emit_addr_constant(dest, address.wrapping_sub(info.address) as i16, true, Some(&global.name))
        } else {
            self.emit_gp_addr(dest, address)
        }
    }

    fn initializer_bank(&self) -> Reg { if self.banked_globals { Reg::T6 } else { Reg::Gp } }

    fn emit_banked_address(&self, addr: Reg, bank: Reg, symbol: &str, cells: i64) -> Vec<AsmInst> {
        let expression = format!("{symbol}+{cells}");
        vec![AsmInst::LiLabel(addr, format!("%data_offset({expression})")),
             AsmInst::LiLabel(bank, format!("%data_bank({expression})")),
             AsmInst::Add(bank, bank, Reg::Gp)]
    }

    /// Allocate space for a global variable
    pub fn allocate_global(&mut self, global: &GlobalVariable) -> GlobalInfo {
        // Calculate size in words (16-bit)
        let size = if global.linkage == Linkage::Declaration { 0 } else {
            u32::try_from(global.var_type.size_in_words().unwrap_or(1)).expect("validated global size")
        };
        
        let info = GlobalInfo {
            address: self.next_address,
            size,
        };
        
        if global.linkage != Linkage::Declaration {
            self.next_address = self.next_address.checked_add(size)
                .expect("global storage exceeds the 32-bit data address space");
            self.definitions.push((global.name.clone(), size));
        }
        self.allocations.insert(global.name.clone(), info.clone());
        
        debug!("Allocated global '{}' at address {} (size: {} words)", 
               global.name, info.address, size);
        
        info
    }
    
    /// Get allocation info for a global variable
    pub fn get_global_info(&self, name: &str) -> Option<&GlobalInfo> {
        self.allocations.get(name)
    }
    
    /// Get all global allocations
    pub fn get_all_allocations(&self) -> &HashMap<String, GlobalInfo> {
        &self.allocations
    }
    
    /// Generate initialization code for a global variable
    pub fn lower_global_init(&self, global: &GlobalVariable, info: &GlobalInfo) -> Vec<AsmInst> {
        let mut insts = Vec::new();
        
        // Generate initialization based on the initializer
        match &global.initializer {
            Some(Value::ConstantAggregate(values)) => {
                insts.extend(self.lower_aggregate_init(global, &global.var_type, values, info.address));
            }
            Some(Value::ConstantArray(values)) => {
                // Initialize array with provided values
                insts.extend(self.lower_array_init(global, values, info.address));
            }
            Some(init_value) => {
                // Initialize with single value (pass self for global lookups)
                insts.extend(self.lower_single_value_init(global, init_value, info.address));
            }
            None => {
                // No initializer - leave uninitialized
                insts.push(AsmInst::Comment(format!("Uninitialized global {}", global.name)));
            }
        }
        
        self.reuse_initializer_cursor(insts)
    }
    
    /// Keep the destination cursor between stores in one initializer. Relocate
    /// its base once, then advance by the difference between local offsets.
    /// Control flow and unknown instructions discard the proof; this does not
    /// move or remove any memory writes (including volatile initialization).
    fn reuse_initializer_cursor(&self, insts: Vec<AsmInst>) -> Vec<AsmInst> {
        if self.banked_globals { return insts; }
        let Some(base) = self.gp_base_label() else { return insts; };
        let mut result = Vec::with_capacity(insts.len());
        let mut cursor: Option<(String, u16)> = None;
        let mut iter = insts.into_iter().peekable();
        while let Some(inst) = iter.next() {
            if matches!(&inst, AsmInst::LiLabel(Reg::T1, label) if *label == base || self.allocations.contains_key(label)) {
                let offset = match iter.peek() {
                    Some(AsmInst::AddI(Reg::T1, Reg::T1, offset)) => {
                        let offset = *offset as u16;
                        iter.next();
                        offset
                    }
                    _ => 0,
                };
                let label = match &inst { AsmInst::LiLabel(_, label) => label.clone(), _ => unreachable!() };
                if let Some((_, previous)) = cursor.as_ref().filter(|(previous, _)| previous == &label) {
                    let delta = offset.wrapping_sub(*previous);
                    if delta != 0 {
                        result.push(AsmInst::AddI(Reg::T1, Reg::T1, delta as i16));
                    }
                } else {
                    result.push(inst);
                    if offset != 0 {
                        result.push(AsmInst::AddI(Reg::T1, Reg::T1, offset as i16));
                    }
                }
                cursor = Some((label, offset));
                continue;
            }
            let preserves_cursor = match &inst {
                AsmInst::Comment(_) | AsmInst::Store(..) => true,
                AsmInst::Li(dest, _) | AsmInst::LiLabel(dest, _)
                | AsmInst::Add(dest, _, _) | AsmInst::AddI(dest, _, _)
                | AsmInst::Sub(dest, _, _) | AsmInst::Sltu(dest, _, _) => *dest != Reg::T1,
                _ => false,
            };
            if !preserves_cursor { cursor = None; }
            result.push(inst);
        }
        result
    }

    /// Lower array initialization
    fn lower_array_init(&self, global: &GlobalVariable, values: &[i64], address: u32) -> Vec<AsmInst> {
        let mut insts = Vec::new();
        
        // Add comment - if it looks like string data, format it nicely
        let is_likely_string = values.last() == Some(&0) && 
            values[..values.len().saturating_sub(1)].iter()
                .all(|&v| (0..=127).contains(&v));
        
        if is_likely_string {
            // Create a safe string representation for the comment
            let safe_str: String = values[..values.len().saturating_sub(1)].iter()
                .map(|&c| match c as u8 {
                    b'\n' => "\\n".to_string(),
                    b'\t' => "\\t".to_string(),
                    b'\r' => "\\r".to_string(),
                    b'\\' => "\\\\".to_string(),
                    c if c.is_ascii_graphic() || c == b' ' => (c as char).to_string(),
                    c => format!("\\x{c:02x}"),
                })
                .collect();
            insts.push(AsmInst::Comment(format!("String data \"{safe_str}\" at address {address}")));
        } else {
            insts.push(AsmInst::Comment(format!("Array {} at address {}", global.name, address)));
        }
        
        // Store each value
        let mut addr = address;
        for &value in values {
            if self.banked_globals && value == 0 { addr += 1; continue; }
            insts.push(AsmInst::Li(Reg::T0, value as i16));
            insts.extend(self.emit_initializer_addr(Reg::T1, global, addr));
            insts.push(AsmInst::Store(Reg::T0, self.initializer_bank(), Reg::T1)); // Store to global memory (bank GP)
            addr += 1;
        }
        
        insts
    }
    
    /// Lower single value initialization
    fn lower_single_value_init(&self, global: &GlobalVariable, init_value: &Value, address: u32) -> Vec<AsmInst> {
        let mut insts = Vec::new();
        
        insts.push(AsmInst::Comment(format!("Global variable: {} at address {}", 
                                            global.name, address)));
        
        match init_value {
            Value::Constant(val) => {
                if self.banked_globals && *val == 0 { return insts; }
                // Handle different sizes
                match &global.var_type {
                    IrType::I64 | IrType::F64 => {
                        let parts = crate::instr::i64::split_const64(*val);
                        for i in 0..4 {
                            insts.push(AsmInst::Li(Reg::T0, parts[i]));
                            insts.extend(self.emit_initializer_addr(Reg::T1, global, address + i as u32));
                            insts.push(AsmInst::Store(Reg::T0, self.initializer_bank(), Reg::T1));
                        }
                    }
                    IrType::I32 | IrType::F32 => {
                        // 32-bit values need two stores
                        let low = (*val & 0xFFFF) as i16;
                        let high = ((*val >> 16) & 0xFFFF) as i16;
                        
                        // Store low word
                        insts.push(AsmInst::Li(Reg::T0, low));
                        insts.extend(self.emit_initializer_addr(Reg::T1, global, address));
                        insts.push(AsmInst::Store(Reg::T0, self.initializer_bank(), Reg::T1));
                        
                        // Store high word
                        insts.push(AsmInst::Li(Reg::T0, high));
                        insts.extend(self.emit_initializer_addr(Reg::T1, global, address + 1));
                        insts.push(AsmInst::Store(Reg::T0, self.initializer_bank(), Reg::T1));
                    }
                    _ => {
                        // Default: single word store
                        insts.push(AsmInst::Li(Reg::T0, *val as i16));
                        insts.extend(self.emit_initializer_addr(Reg::T1, global, address));
                        insts.push(AsmInst::Store(Reg::T0, self.initializer_bank(), Reg::T1));
                    }
                }
            }
            Value::FatPtr(fp) => {
                // For FatPtr initializers (e.g., pointer to string literal)
                // Store the address component
                match fp.addr.as_ref() {
                    Value::AddressOffset { base, cells } => {
                        insts.extend(self.lower_address_pointer_init(global, base, fp.bank, *cells, address));
                    }
                    Value::Function(name) => {
                        insts.extend(self.lower_function_pointer_init(global, name, address));
                    }
                    Value::Global(name) => {
                        if self.banked_globals {
                            return self.lower_address_pointer_init(global, &Value::Global(name.clone()), rcc_ir::BankTag::Global, 0, address);
                        }
                        // Need to get the address of the referenced global
                        insts.push(AsmInst::Comment(format!("Pointer to global '{}'", name)));
                        
                        // Look up the address of the referenced global
                        debug!("Looking up global '{}' for pointer initialization", name);
                        if let Some(target_info) = self.get_global_info(name) {
                            debug!("Found global '{}' at address {}", name, target_info.address);
                            // Store the (relocatable) address of the target global
                            let _ = target_info;
                            insts.push(AsmInst::LiLabel(Reg::T0, name.clone()));
                            insts.extend(self.emit_initializer_addr(Reg::T1, global, address));
                            insts.push(AsmInst::Store(Reg::T0, self.initializer_bank(), Reg::T1));
                            
                            // Store the bank (GP register value for global)
                            insts.push(AsmInst::Add(Reg::T0, Reg::Gp, Reg::R0)); // Copy GP register value
                            insts.extend(self.emit_initializer_addr(Reg::T1, global, address + 1));
                            insts.push(AsmInst::Store(Reg::T0, self.initializer_bank(), Reg::T1));
                        } else {
                            // Global not found - this is a compiler error
                            panic!("COMPILER ERROR: Global pointer '{}' references undefined global '{}'", 
                                   global.name, name);
                        }
                    }
                    Value::Constant(val) => {
                        if self.banked_globals && fp.bank == rcc_ir::BankTag::Global {
                            return self.lower_address_pointer_init(global, fp.addr.as_ref(), fp.bank, 0, address);
                        }
                        // Direct constant pointer value
                        if matches!(fp.bank, rcc_ir::BankTag::Global) {
                            insts.extend(self.emit_gp_addr(Reg::T0, *val as u32));
                        } else {
                            insts.push(AsmInst::Li(Reg::T0, *val as i16));
                        }
                        insts.extend(self.emit_initializer_addr(Reg::T1, global, address));
                        insts.push(AsmInst::Store(Reg::T0, self.initializer_bank(), Reg::T1));
                        
                        // Store the bank component
                        match fp.bank {
                            rcc_ir::BankTag::Global => {
                                insts.push(AsmInst::Add(Reg::T0, Reg::Gp, Reg::R0)); // Copy GP register
                            }
                            rcc_ir::BankTag::Stack => {
                                insts.push(AsmInst::Add(Reg::T0, Reg::Sb, Reg::R0)); // Copy SB register
                            }
                            rcc_ir::BankTag::Null => {
                                insts.push(AsmInst::Li(Reg::T0, crate::regmgmt::BankTagValue::NULL));
                            }
                            rcc_ir::BankTag::Heap(bank) => insts.push(AsmInst::Li(Reg::T0, bank as i16)),
                            _ => panic!("unsupported constant global pointer bank"),
                        }
                        insts.extend(self.emit_initializer_addr(Reg::T1, global, address + 1));
                        insts.push(AsmInst::Store(Reg::T0, self.initializer_bank(), Reg::T1));
                    }
                    _ => {
                        panic!("COMPILER ERROR: Global pointer '{}' has unsupported address type: {:?}", 
                               global.name, fp.addr);
                    }
                }
            }
            Value::Function(name) => insts.extend(self.lower_function_pointer_init(global, name, address)),
            Value::Global(name) => {
                // Direct reference to another global (for non-pointer types)
                panic!("COMPILER ERROR: Global '{}' initialized with reference to '{}' - not yet supported", 
                       global.name, name);
            }
            _ => {
                // Other initializer types not yet supported
                panic!("COMPILER ERROR: Global '{}' has unsupported initializer type: {:?}", 
                       global.name, init_value);
            }
        }
        
        insts
    }
    fn lower_aggregate_init(&self, global: &GlobalVariable, ty: &IrType, values: &[Value], address: u32) -> Vec<AsmInst> {
        // C unions use raw I16-array storage. The frontend splits scalar
        // members into cells but retains each relocatable pointer as one
        // two-cell initializer atom, so element count is not cell count.
        if matches!(ty, IrType::Array { element_type, .. } if **element_type == IrType::I16) &&
            values.iter().all(|v| matches!(v, Value::Constant(_) | Value::FatPtr(_) | Value::Function(_) | Value::Global(_))) {
            let mut insts = Vec::new();
            let mut offset = 0u32;
            let capacity = ty.size_in_words().expect("sized cell initializer");
            for value in values {
                let pointer = matches!(value, Value::FatPtr(_) | Value::Function(_) | Value::Global(_));
                let width = if pointer { 2 } else { 1 };
                assert!(u64::from(offset + width) <= capacity, "cell initializer exceeds object storage");
                let mut element = global.clone();
                element.var_type = if pointer { IrType::FatPtr(Box::new(IrType::I16)) } else { IrType::I16 };
                insts.extend(self.lower_single_value_init(&element, value, address + offset));
                offset += width;
            }
            return insts;
        }
        let mut values = values.iter().peekable();
        let insts = self.lower_aggregate_elements(global, ty, &mut values, address);
        assert!(values.next().is_none(), "too many global aggregate initializers");
        insts
    }

    fn lower_aggregate_elements<'a>(&self, global: &GlobalVariable, ty: &IrType, values: &mut std::iter::Peekable<std::slice::Iter<'a, Value>>, address: u32) -> Vec<AsmInst> {
        let element_types: Vec<&IrType> = match ty {
            IrType::Array { size, element_type } => (0..*size).map(|_| element_type.as_ref()).collect(),
            IrType::Struct { fields, .. } => fields.iter().collect(),
            _ => vec![ty],
        };
        let mut insts = Vec::new();
        let mut offset = address;
        for element_type in element_types {
            let Some(value) = values.peek().copied() else { break; };
            match value {
                Value::ConstantAggregate(nested) => {
                    values.next();
                    insts.extend(self.lower_aggregate_init(global, element_type, nested, offset));
                }
                Value::ConstantArray(cells) => {
                    values.next();
                    insts.extend(self.lower_array_init(global, cells, offset));
                }
                _ if matches!(element_type, IrType::Array { .. } | IrType::Struct { .. }) => {
                    // C permits brace elision: int a[2][2] = {1,2,3,4}.
                    insts.extend(self.lower_aggregate_elements(global, element_type, values, offset));
                }
                _ => {
                    values.next();
                    let mut element = global.clone();
                    element.var_type = element_type.clone();
                    insts.extend(self.lower_single_value_init(&element, value, offset));
                }
            }
            offset += element_type.size_in_words().expect("sized global initializer") as u32;
        }
        insts
    }

    fn lower_function_pointer_init(&self, global: &GlobalVariable, name: &str, address: u32) -> Vec<AsmInst> {
        let mut insts = vec![AsmInst::LiLabel(Reg::T0, format!("%offset({name})"))];
        insts.extend(self.emit_initializer_addr(Reg::T1, global, address));
        insts.push(AsmInst::Store(Reg::T0, self.initializer_bank(), Reg::T1));
        insts.push(AsmInst::LiLabel(Reg::T0, format!("%bank({name})")));
        insts.extend(self.emit_initializer_addr(Reg::T1, global, address + 1));
        insts.push(AsmInst::Store(Reg::T0, self.initializer_bank(), Reg::T1));
        insts
    }

    /// Relocate the base first, then add the displacement to the complete fat
    /// pointer. A 16-bit addition alone loses both bank-size and CPU-word carry.
    fn lower_address_pointer_init(&self, global: &GlobalVariable, base: &Value, bank: rcc_ir::BankTag, cells: i64, address: u32) -> Vec<AsmInst> {
        use rcc_ir::BankTag;
        if self.banked_globals && bank == BankTag::Global {
            let (symbol, displacement) = match base {
                Value::Global(name) => (name.clone(), cells),
                Value::Constant(n) => (self.gp_base_label().expect("allocated globals"), n.checked_add(cells).expect("global address displacement overflow")),
                _ => panic!("invalid static address base: {base:?}"),
            };
            let mut insts = self.emit_banked_address(Reg::T0, Reg::T2, &symbol, displacement);
            insts.extend(self.emit_initializer_addr(Reg::T1, global, address));
            insts.push(AsmInst::Store(Reg::T0, self.initializer_bank(), Reg::T1));
            insts.extend(self.emit_initializer_addr(Reg::T1, global, address + 1));
            insts.push(AsmInst::Store(Reg::T2, self.initializer_bank(), Reg::T1));
            return insts;
        }
        let mut insts = match base {
            Value::Global(name) => vec![AsmInst::LiLabel(Reg::T0, name.clone())],
            Value::Constant(offset) if bank == BankTag::Global => self.emit_gp_addr(Reg::T0, *offset as u32),
            Value::Constant(offset) => vec![AsmInst::Li(Reg::T0, *offset as i16)],
            _ => panic!("invalid static address base: {base:?}"),
        };
        insts.push(match bank {
            BankTag::Global => AsmInst::Add(Reg::T2, Reg::Gp, Reg::R0),
            BankTag::Stack => AsmInst::Add(Reg::T2, Reg::Sb, Reg::R0),
            BankTag::Heap(bank) => AsmInst::Li(Reg::T2, bank as i16),
            BankTag::Null => AsmInst::Li(Reg::T2, crate::regmgmt::BankTagValue::NULL),
            _ => panic!("invalid static address bank: {bank:?}"),
        });
        let quotient = cells.div_euclid(i64::from(self.bank_size));
        let remainder = cells.rem_euclid(i64::from(self.bank_size)) as u16;
        if quotient != 0 {
            insts.push(AsmInst::AddI(Reg::T2, Reg::T2, quotient as i16));
        }
        if remainder != 0 {
            let adjust = format!("L_address_{}_{}_adjust", self.unit_tag(), address);
            let done = format!("L_address_{}_{}_done", self.unit_tag(), address);
            insts.push(AsmInst::AddI(Reg::T3, Reg::T0, remainder as i16));
            insts.push(AsmInst::Sltu(Reg::T4, Reg::T3, Reg::T0));
            insts.push(AsmInst::Li(Reg::T5, self.bank_size as i16));
            insts.push(AsmInst::Bne(Reg::T4, Reg::R0, adjust.clone()));
            insts.push(AsmInst::Sltu(Reg::T4, Reg::T3, Reg::T5));
            insts.push(AsmInst::Bne(Reg::T4, Reg::R0, done.clone()));
            insts.push(AsmInst::Label(adjust));
            insts.push(AsmInst::Sub(Reg::T3, Reg::T3, Reg::T5));
            insts.push(AsmInst::AddI(Reg::T2, Reg::T2, 1));
            insts.push(AsmInst::Label(done));
            insts.push(AsmInst::Add(Reg::T0, Reg::T3, Reg::R0));
        }
        insts.extend(self.emit_initializer_addr(Reg::T1, global, address));
        insts.push(AsmInst::Store(Reg::T0, self.initializer_bank(), Reg::T1));
        insts.extend(self.emit_initializer_addr(Reg::T1, global, address + 1));
        insts.push(AsmInst::Store(Reg::T2, self.initializer_bank(), Reg::T1));
        insts
    }

}

impl Default for GlobalManager {
    fn default() -> Self {
        Self::new()
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use rcc_ir::ir::Linkage;

    fn allocated_manager() -> GlobalManager {
        let mut manager = GlobalManager::new();
        manager.allocate_global(&GlobalVariable {
            name: "data".into(), var_type: IrType::I16, is_constant: false,
            initializer: None, linkage: Linkage::External, symbol_id: None,
        });
        manager
    }

    #[test]
    fn initializer_cursor_reuses_relocated_base_and_wrapping_deltas() {
        let manager = allocated_manager();
        let base = manager.gp_base_label().unwrap();
        let offsets = [0u16, 1, 1, 32768, 63999, 0, 40000];
        let mut original = Vec::new();
        for offset in offsets {
            original.push(AsmInst::Li(Reg::T0, offset as i16));
            original.extend(manager.emit_gp_addr(Reg::T1, u32::from(offset)));
            original.push(AsmInst::Store(Reg::T0, Reg::Gp, Reg::T1));
        }
        let optimized = manager.reuse_initializer_cursor(original.clone());
        assert_eq!(optimized.iter().filter(|i| matches!(i, AsmInst::LiLabel(Reg::T1, _))).count(), 1);
        // An independent tiny evaluator checks every emitted store at several
        // relocated bases, including carries through the 16-bit word boundary.
        let stores = |code: &[AsmInst], relocated: u16| {
            let (mut value, mut address) = (0u16, 0u16);
            let mut writes = Vec::new();
            for inst in code {
                match inst {
                    AsmInst::Li(Reg::T0, v) => value = *v as u16,
                    AsmInst::LiLabel(Reg::T1, label) if label == &base => address = relocated,
                    AsmInst::AddI(Reg::T1, Reg::T1, n) => address = address.wrapping_add(*n as u16),
                    AsmInst::Store(Reg::T0, Reg::Gp, Reg::T1) => writes.push((address, value)),
                    other => panic!("unexpected fixture instruction: {other:?}"),
                }
            }
            writes
        };
        for relocated in [0, 42, 32000, 65535] {
            assert_eq!(stores(&original, relocated), stores(&optimized, relocated));
        }
    }

    #[test]
    fn initializer_cursor_forgets_clobbers_control_flow_and_unknown_code() {
        let manager = allocated_manager();
        for barrier in [
            AsmInst::Li(Reg::T1, 123), AsmInst::AddI(Reg::T1, Reg::T1, 1),
            AsmInst::Label("join".into()), AsmInst::Bne(Reg::T0, Reg::R0, "join".into()),
            AsmInst::Call("other".into()), AsmInst::Raw("NOP".into()),
        ] {
            let mut original = manager.emit_gp_addr(Reg::T1, 10);
            original.push(barrier);
            original.extend(manager.emit_gp_addr(Reg::T1, 11));
            assert_eq!(manager.reuse_initializer_cursor(original.clone()), original);
        }
    }
    
    #[test]
    fn test_global_allocation() {
        let mut manager = GlobalManager::new();
        
        let global1 = GlobalVariable {
            name: "global_x".to_string(),
            var_type: IrType::I16,
            is_constant: false,
            initializer: Some(Value::Constant(42)),
            linkage: Linkage::External,
            symbol_id: None,
        };
        
        let global2 = GlobalVariable {
            name: "global_y".to_string(),
            var_type: IrType::I32,
            is_constant: false,
            initializer: Some(Value::Constant(0x12345678)),
            linkage: Linkage::External,
            symbol_id: None,
        };
        
        let info1 = manager.allocate_global(&global1);
        let info2 = manager.allocate_global(&global2);
        
        assert_eq!(info1.address, 0);
        assert_eq!(info1.size, 1);
        assert_eq!(info2.address, 1);
        assert_eq!(info2.size, 2);
    }
    
    #[test]
    fn test_string_literal_allocation() {
        let mut manager = GlobalManager::new();
        
        // "Hi" = 0x48 0x69, but we'll have 3 words (H, i, \0)
        let global = GlobalVariable {
            name: "__str_0_4869".to_string(),
            var_type: IrType::Array {
                element_type: Box::new(IrType::I8),
                size: 3,
            },
            is_constant: true,
            initializer: None,
            linkage: Linkage::Internal,
            symbol_id: None,
        };
        
        let info = manager.allocate_global(&global);
        assert_eq!(info.address, 0);
        assert_eq!(info.size, 3); // One word per character including null
    }
    
    #[test]
    fn test_global_initialization_code() {
        let global = GlobalVariable {
            name: "test_var".to_string(),
            var_type: IrType::I16,
            is_constant: false,
            initializer: Some(Value::Constant(100)),
            linkage: Linkage::External,
            symbol_id: None,
        };
        
        let info = GlobalInfo { address: 10, size: 1 };
        let manager = GlobalManager::new();
        let insts = manager.lower_global_init(&global, &info);
        
        // Should have: comment, Li(T0, 100), Li(T1, 10), Store
        assert!(insts.iter().any(|i| matches!(i, AsmInst::Comment(_))));
        assert!(insts.iter().any(|i| matches!(i, AsmInst::Li(Reg::T0, 100))));
        // Without allocated globals there is no GP base label, so the address is numeric
        assert!(insts.iter().any(|i| matches!(i, AsmInst::Li(Reg::T1, 10))));
        assert!(insts.iter().any(|i| matches!(i, AsmInst::Store(Reg::T0, Reg::Gp, Reg::T1))));
    }
}
