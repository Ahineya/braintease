use crate::types::{Instruction, Label, ObjectFile, Opcode, UnresolvedReference, Archive, ArchiveEntry};
use std::collections::HashMap;
use std::path::Path;
use std::fs;

pub struct Linker {
    bank_size: u16,
}

impl Linker {
    pub fn new(bank_size: u16) -> Self {
        Self { bank_size }
    }
    
    /// Create an archive from multiple object files
    pub fn create_archive(object_files: Vec<(String, ObjectFile)>) -> Archive {
        Archive {
            version: 1,
            objects: object_files.into_iter()
                .map(|(name, object)| ArchiveEntry { name, object })
                .collect(),
        }
    }
    
    /// Extract object files from an archive
    pub fn extract_from_archive(archive: &Archive) -> Vec<ObjectFile> {
        archive.objects.iter()
            .map(|entry| entry.object.clone())
            .collect()
    }

    pub fn link(&self, object_files: Vec<ObjectFile>) -> Result<LinkedProgram, Vec<String>> {
        let mut errors = Vec::new();
        let bank_size = self.bank_size.max(1) as usize;

        // Pack objects (or functions, if an object does not fit in one bank)
        // so instruction index = PCB * bank_size + PC. Pad unused bank tails
        // with NOP (not HALT: opcode 0 with all-zero operands is HALT).
        let mut packed_instructions = Vec::new();
        let mut file_maps: Vec<Vec<usize>> = Vec::with_capacity(object_files.len());

        for (file_idx, obj) in object_files.iter().enumerate() {
            let mut local_map = vec![0usize; obj.instructions.len()];
            if obj.instructions.is_empty() {
                file_maps.push(local_map);
                continue;
            }

            let segments = match self.segments_for_object(obj, bank_size) {
                Ok(segs) => segs,
                Err(e) => {
                    errors.push(format!("File {}: {}", file_idx, e));
                    file_maps.push(local_map);
                    continue;
                }
            };

            for segment in segments {
                let len = segment.end - segment.start;
                if !packed_instructions.is_empty() {
                    let used = packed_instructions.len() % bank_size;
                    let remainder = if used == 0 { bank_size } else { bank_size - used };
                    if len > remainder {
                        Self::pad_to_bank_end(&mut packed_instructions, bank_size);
                    }
                }

                for local_idx in segment.start..segment.end {
                    local_map[local_idx] = packed_instructions.len();
                    packed_instructions.push(obj.instructions[local_idx]);
                }
            }

            file_maps.push(local_map);
        }

        // Concatenate data independently of code banking
        let mut all_data = Vec::new();
        let mut global_labels = HashMap::new();
        let mut global_data_labels = HashMap::new();

        for (file_idx, obj) in object_files.iter().enumerate() {
            let file_data_start = all_data.len() as u32;
            all_data.extend_from_slice(&obj.data);

            for (name, label) in &obj.labels {
                let local_inst = (label.absolute_address / 4) as usize;
                let mut adjusted_label = label.clone();
                if local_inst < file_maps[file_idx].len() {
                    let new_inst = file_maps[file_idx][local_inst];
                    adjusted_label.absolute_address = (new_inst * 4) as u32;
                    adjusted_label.bank = (new_inst / bank_size) as u16;
                    adjusted_label.offset = ((new_inst % bank_size) * 4) as u16;
                }

                if global_labels.insert(name.clone(), adjusted_label).is_some() {
                    errors.push(format!("Duplicate label '{}' in file {}", name, file_idx));
                }
            }

            for (name, offset) in &obj.data_labels {
                let adjusted_offset = offset + file_data_start;
                if global_data_labels.insert(name.clone(), adjusted_offset).is_some() {
                    errors.push(format!("Duplicate data label '{}' in file {}", name, file_idx));
                }
            }
        }

        if !errors.is_empty() {
            return Err(errors);
        }

        let mut resolved_instructions = packed_instructions;

        for (file_idx, obj) in object_files.iter().enumerate() {
            for (local_idx, unresolved) in &obj.unresolved_references {
                if *local_idx >= file_maps[file_idx].len() {
                    errors.push(format!(
                        "File {}, instruction {}: unresolved reference index out of range",
                        file_idx, local_idx
                    ));
                    continue;
                }
                let global_idx = file_maps[file_idx][*local_idx];

                if let Err(e) = self.resolve_reference(
                    &mut resolved_instructions[global_idx],
                    unresolved,
                    &global_labels,
                    &global_data_labels,
                    global_idx,
                ) {
                    errors.push(format!("File {}, instruction {}: {}", file_idx, local_idx, e));
                }
            }

            // Remap assembler-resolved local JALs to (bank, in-bank offset)
            for (local_idx, instruction) in obj.instructions.iter().enumerate() {
                if instruction.opcode != Opcode::Jal as u8 {
                    continue;
                }
                if obj.unresolved_references.contains_key(&local_idx) {
                    continue;
                }
                let local_target = instruction.word3 as usize;
                if local_target >= obj.instructions.len() {
                    continue;
                }
                let new_target = file_maps[file_idx][local_target];
                let global_idx = file_maps[file_idx][local_idx];
                Self::encode_jal_target(&mut resolved_instructions[global_idx], new_target, bank_size);
            }
        }

        if !errors.is_empty() {
            return Err(errors);
        }

        let entry_point = object_files.iter()
            .filter_map(|obj| obj.entry_point.as_ref())
            .next()
            .and_then(|name| global_labels.get(name))
            .map(|label| label.absolute_address)
            .unwrap_or(0);

        Ok(LinkedProgram {
            instructions: resolved_instructions,
            data: all_data,
            labels: global_labels,
            data_labels: global_data_labels,
            entry_point,
            bank_size: self.bank_size,
        })
    }

    /// Compiler block labels are `L_{func}_{id}`. Everything else is treated as
    /// a function entry so a translation unit can be split without cutting a
    /// function (and its BEQ/BNE offsets) across a bank.
    fn is_function_entry_label(name: &str) -> bool {
        !name.starts_with("L_") && !name.starts_with('.')
    }

    fn nop_pad_instruction() -> Instruction {
        Instruction::new(Opcode::Nop, 1, 0, 0)
    }

    fn pad_to_bank_end(instructions: &mut Vec<Instruction>, bank_size: usize) {
        let used = instructions.len() % bank_size;
        if used == 0 {
            return;
        }
        let pad = bank_size - used;
        for _ in 0..pad {
            instructions.push(Self::nop_pad_instruction());
        }
    }

    fn encode_jal_target(instruction: &mut Instruction, inst_idx: usize, bank_size: usize) {
        instruction.word2 = (inst_idx / bank_size) as u16;
        instruction.word3 = (inst_idx % bank_size) as u16;
    }

    fn segments_for_object(
        &self,
        obj: &ObjectFile,
        bank_size: usize,
    ) -> Result<Vec<CodeSegment>, String> {
        let len = obj.instructions.len();
        if len <= bank_size {
            return Ok(vec![CodeSegment { start: 0, end: len, name: None }]);
        }

        let mut starts: Vec<(usize, String)> = obj.labels.iter()
            .filter(|(name, _)| Self::is_function_entry_label(name))
            .map(|(name, label)| ((label.absolute_address / 4) as usize, name.clone()))
            .filter(|(idx, _)| *idx < len)
            .collect();
        starts.sort_by_key(|(idx, _)| *idx);
        starts.dedup_by_key(|(idx, _)| *idx);

        if starts.is_empty() {
            return Err(format!(
                "object is {} instructions, larger than bank size {}, and has no function labels to split on",
                len, bank_size
            ));
        }

        if starts[0].0 != 0 {
            starts.insert(0, (0, "<anonymous>".to_string()));
        }

        let mut segments = Vec::new();
        for i in 0..starts.len() {
            let start = starts[i].0;
            let end = if i + 1 < starts.len() { starts[i + 1].0 } else { len };
            let name = starts[i].1.clone();
            let seg_len = end - start;
            if seg_len > bank_size {
                return Err(format!(
                    "function '{}' is {} instructions, larger than bank size {}",
                    name, seg_len, bank_size
                ));
            }
            segments.push(CodeSegment { start, end, name: Some(name) });
        }

        Ok(segments)
    }

    fn resolve_reference(
        &self,
        instruction: &mut Instruction,
        reference: &UnresolvedReference,
        labels: &HashMap<String, Label>,
        data_labels: &HashMap<String, u32>,
        current_idx: usize,
    ) -> Result<(), String> {
        match reference.ref_type.as_str() {
            "branch" => {
                // For branch instructions, calculate relative offset
                let target_label = labels.get(&reference.label)
                    .ok_or_else(|| format!("Undefined label: {}", reference.label))?;
                
                // Both addresses should be in instruction indices, not bytes
                let current_inst = current_idx as i32;
                let target_inst = (target_label.absolute_address / 4) as i32;
                let offset = target_inst - current_inst;
                
                // Update the immediate field (word3 for branches)
                instruction.word3 = offset as u16;
            }
            "absolute" => {
                // JAL: word2 = code bank, word3 = in-bank instruction offset
                let bank_size = self.bank_size.max(1) as usize;
                if let Some(label) = labels.get(&reference.label) {
                    let instruction_idx = (label.absolute_address / 4) as usize;
                    Self::encode_jal_target(instruction, instruction_idx, bank_size);
                } else if let Some(&data_addr) = data_labels.get(&reference.label) {
                    instruction.word2 = (data_addr >> 16) as u16;
                    instruction.word3 = (data_addr & 0xFFFF) as u16;
                } else {
                    return Err(format!("Undefined label: {}", reference.label));
                }
            }
            "data" => {
                // For data references (LOAD/STORE/LI with labels)
                let data_addr = data_labels.get(&reference.label)
                    .ok_or_else(|| format!("Undefined data label: {}", reference.label))?;
                
                // Check if this is an LI instruction (opcode 0x0E)
                if instruction.opcode == 0x0E {
                    // For LI, the immediate value goes in word2
                    instruction.word2 = *data_addr as u16;
                } else {
                    // For LOAD/STORE, the address goes in word3
                    instruction.word3 = *data_addr as u16;
                }
            }
            _ => {
                return Err(format!("Unknown reference type: {}", reference.ref_type));
            }
        }
        
        Ok(())
    }

    pub fn link_files(&self, paths: &[&Path]) -> Result<LinkedProgram, Vec<String>> {
        let mut object_files = Vec::new();
        let mut errors = Vec::new();

        for path in paths {
            match fs::read_to_string(path) {
                Ok(content) => {
                    match serde_json::from_str::<ObjectFile>(&content) {
                        Ok(obj) => object_files.push(obj),
                        Err(e) => errors.push(format!("Failed to parse {}: {}", path.display(), e)),
                    }
                }
                Err(e) => errors.push(format!("Failed to read {}: {}", path.display(), e)),
            }
        }

        if !errors.is_empty() {
            return Err(errors);
        }

        self.link(object_files)
    }
}

struct CodeSegment {
    start: usize,
    end: usize,
    #[allow(dead_code)]
    name: Option<String>,
}

#[derive(Debug)]
pub struct LinkedProgram {
    pub instructions: Vec<Instruction>,
    pub data: Vec<u8>,
    pub labels: HashMap<String, Label>,
    pub data_labels: HashMap<String, u32>,
    pub entry_point: u32,
    pub bank_size: u16,
}

impl LinkedProgram {
    /// Number of code banks occupied after packing (ceil(len / bank_size)).
    pub fn code_bank_count(&self) -> usize {
        let bs = self.bank_size.max(1) as usize;
        if self.instructions.is_empty() {
            0
        } else {
            (self.instructions.len() + bs - 1) / bs
        }
    }

    pub fn to_binary(&self) -> Vec<u8> {
        let mut binary = Vec::new();
        
        // Write magic number for linked program
        binary.extend_from_slice(b"RLINK");
        
        // Write bank size (new field in binary format)
        binary.extend_from_slice(&self.bank_size.to_le_bytes());
        
        // Write entry point
        binary.extend_from_slice(&self.entry_point.to_le_bytes());
        
        // Write instruction count
        binary.extend_from_slice(&(self.instructions.len() as u32).to_le_bytes());
        
        // Write instructions
        for inst in &self.instructions {
            binary.push(inst.opcode);
            binary.push(inst.word0);
            binary.extend_from_slice(&inst.word1.to_le_bytes());
            binary.extend_from_slice(&inst.word2.to_le_bytes());
            binary.extend_from_slice(&inst.word3.to_le_bytes());
        }
        
        // Write data section size
        binary.extend_from_slice(&(self.data.len() as u32).to_le_bytes());
        
        // Write data
        binary.extend_from_slice(&self.data);
        
        // Write debug info section
        // Filter labels to only include function-like labels (no dots or underscores at start)
        let function_labels: Vec<(&String, &Label)> = self.labels.iter()
            .filter(|(name, _)| {
                // Include labels that look like functions (no dots, no leading underscores)
                !name.starts_with('_') && !name.starts_with('.') && !name.contains('.')
            })
            .collect();
        
        // Write debug section marker
        binary.extend_from_slice(b"DEBUG");
        
        // Write number of debug entries
        binary.extend_from_slice(&(function_labels.len() as u32).to_le_bytes());
        
        // Write each debug entry
        for (name, label) in function_labels {
            // Write name length
            binary.extend_from_slice(&(name.len() as u32).to_le_bytes());
            // Write name
            binary.extend_from_slice(name.as_bytes());
            // Write instruction index (not byte address)
            let instruction_idx = label.absolute_address / 4;
            binary.extend_from_slice(&instruction_idx.to_le_bytes());
        }
        
        binary
    }

    pub fn to_text(&self) -> String {
        let mut output = String::new();
        
        output.push_str(&format!("; Linked Program\n"));
        output.push_str(&format!("; Bank size: {}\n", self.bank_size));
        output.push_str(&format!("; Entry point: 0x{:08X}\n", self.entry_point));
        output.push_str(&format!("; Instructions: {}\n", self.instructions.len()));
        output.push_str(&format!("; Data size: {} bytes\n\n", self.data.len()));
        
        // Output instructions with addresses
        for (idx, inst) in self.instructions.iter().enumerate() {
            let addr = idx * 4;
            output.push_str(&format!(
                "{:08X}: {:02X} {:02X} {:04X} {:04X} {:04X}\n",
                addr, inst.opcode, inst.word0, inst.word1, inst.word2, inst.word3
            ));
        }
        
        // Output data section
        if !self.data.is_empty() {
            output.push_str("\n; Data Section:\n");
            for (idx, chunk) in self.data.chunks(16).enumerate() {
                output.push_str(&format!("{:08X}: ", idx * 16));
                for byte in chunk {
                    output.push_str(&format!("{:02X} ", byte));
                }
                output.push_str("\n");
            }
        }
        
        output
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::assembler::RippleAssembler;
    use crate::types::{AssemblerOptions, Opcode};

    #[test]
    fn test_link_single_file() {
        let assembler = RippleAssembler::new(AssemblerOptions::default());
        let source = r#"
start:
    LI R3, 42
    HALT
"#;
        let obj = assembler.assemble(source).unwrap();
        
        let linker = Linker::new(16);
        let linked = linker.link(vec![obj]).unwrap();
        
        assert_eq!(linked.instructions.len(), 2);
        assert_eq!(linked.entry_point, 0);
    }

    #[test]
    fn test_link_multiple_files() {
        let assembler = RippleAssembler::new(AssemblerOptions::default());
        
        let source1 = r#"
start:
    JAL RA, R0, func
    HALT
"#;
        let obj1 = assembler.assemble(source1).unwrap();
        
        let source2 = r#"
func:
    LI R3, 42
    RET
"#;
        let obj2 = assembler.assemble(source2).unwrap();
        
        let linker = Linker::new(16);
        let linked = linker.link(vec![obj1, obj2]).unwrap();
        
        assert_eq!(linked.instructions.len(), 4); // JAL + HALT + LI + RET
        assert!(linked.labels.contains_key("start"));
        assert!(linked.labels.contains_key("func"));
    }

    #[test]
    fn test_pack_objects_across_banks() {
        let assembler = RippleAssembler::new(AssemblerOptions::default());

        // 12 instructions: 10 NOP + CALL + HALT. Fits in bank 0 of size 16.
        let mut src1 = String::from("start:\n");
        for _ in 0..10 {
            src1.push_str("    NOP\n");
        }
        src1.push_str("    CALL func\n    HALT\n");
        let obj1 = assembler.assemble(&src1).unwrap();
        assert_eq!(obj1.instructions.len(), 12);

        // 6 instructions: does not fit in the 4-instruction remainder of bank 0.
        let mut src2 = String::from("func:\n");
        for _ in 0..4 {
            src2.push_str("    NOP\n");
        }
        src2.push_str("    LI R3, 42\n    RET\n");
        let obj2 = assembler.assemble(&src2).unwrap();
        assert_eq!(obj2.instructions.len(), 6);

        let linker = Linker::new(16);
        let linked = linker.link(vec![obj1, obj2]).unwrap();

        assert_eq!(linked.code_bank_count(), 2);
        assert_eq!(linked.instructions.len(), 22); // 12 + 4 pad + 6

        // Padding must be NOP, not HALT
        for inst in &linked.instructions[12..16] {
            assert_eq!(inst.opcode, Opcode::Nop as u8);
            assert!(!inst.is_halt());
        }

        let func = linked.labels.get("func").unwrap();
        assert_eq!(func.bank, 1);
        assert_eq!(func.absolute_address, 16 * 4);
        assert_eq!(func.offset, 0);

        // CALL is the 11th instruction (index 10)
        let jal = &linked.instructions[10];
        assert_eq!(jal.opcode, Opcode::Jal as u8);
        assert_eq!(jal.word2, 1); // bank
        assert_eq!(jal.word3, 0); // in-bank offset
    }

    #[test]
    fn test_function_larger_than_bank_fails() {
        let assembler = RippleAssembler::new(AssemblerOptions::default());
        let mut src = String::from("huge:\n");
        for _ in 0..10 {
            src.push_str("    NOP\n");
        }
        let obj = assembler.assemble(&src).unwrap();

        let linker = Linker::new(8);
        let err = linker.link(vec![obj]).unwrap_err();
        assert!(err.iter().any(|e| e.contains("larger than bank size")));
    }

    #[test]
    fn test_split_functions_in_oversized_object() {
        let assembler = RippleAssembler::new(AssemblerOptions::default());
        let mut src = String::from("foo:\n");
        for _ in 0..5 {
            src.push_str("    NOP\n");
        }
        src.push_str("bar:\n");
        for _ in 0..5 {
            src.push_str("    NOP\n");
        }
        let obj = assembler.assemble(&src).unwrap();
        assert_eq!(obj.instructions.len(), 10);

        let linker = Linker::new(8);
        let linked = linker.link(vec![obj]).unwrap();

        assert_eq!(linked.code_bank_count(), 2);
        assert_eq!(linked.labels.get("foo").unwrap().bank, 0);
        assert_eq!(linked.labels.get("bar").unwrap().bank, 1);
        assert_eq!(linked.labels.get("bar").unwrap().absolute_address, 8 * 4);
    }
}