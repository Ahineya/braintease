use crate::types::{Instruction, Label, ObjectFile, Opcode, Register, UnresolvedReference, Archive, ArchiveEntry};
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

    pub fn link(&self, mut object_files: Vec<ObjectFile>) -> Result<LinkedProgram, Vec<String>> {
        crate::reachability::collect(&mut object_files)?;
        let mut dispatcher_index = None;
        // Startup has always called _init_globals. Build one ordinary ISA routine
        // that visits every separately compiled object's initializer.
        if !object_files.iter().any(|obj| obj.labels.contains_key("_init_globals")) {
            let mut names: Vec<_> = object_files.iter().flat_map(|obj| obj.labels.iter()
                .filter(|(name, label)| name.starts_with("__rcc_init_")
                    && !Self::returns_immediately(obj, label))
                .map(|(name, _)| name.clone())).collect();
            names.sort();
            let needs_dispatcher = !names.is_empty() || object_files.iter().any(|obj|
                obj.unresolved_references.values().any(|r| r.label == "_init_globals"));
            if needs_dispatcher {
                let mut source = "_init_globals:\n".to_string();
                if !names.is_empty() {
                    source.push_str("STORE RA, SB, SP\nADDI SP, SP, 1\nSTORE RAB, SB, SP\nADDI SP, SP, 1\n");
                    for name in names { source.push_str(&format!("CALL {name}\n")); }
                    source.push_str("ADDI SP, SP, -1\nLOAD RAB, SB, SP\nADDI SP, SP, -1\nLOAD RA, SB, SP\n");
                }
                source.push_str("RET\n");
                let assembler = crate::assembler::RippleAssembler::new(crate::types::AssemblerOptions::default());
                dispatcher_index = Some(object_files.len());
                object_files.push(assembler.assemble(&source)?);
            }
        }
        let mut errors = Vec::new();
        let bank_size = self.bank_size.max(1) as usize;

        // Pack objects (or functions, if an object does not fit in one bank)
        // so instruction index = PCB * bank_size + PC. Pad unused bank tails
        // with NOP 0x42 (HALT is opcode 0x00).
        let mut packed_instructions = Vec::new();
        let mut file_maps: Vec<Vec<usize>> = object_files.iter()
            .map(|obj| vec![0; obj.instructions.len()]).collect();
        let startup_layout = dispatcher_index.and_then(|index|
            self.startup_layout(&object_files, index, bank_size));
        let layout = match startup_layout {
            Some(layout) => layout,
            None => {
                let mut layout = Vec::new();
                for (file_idx, obj) in object_files.iter().enumerate() {
                    if obj.instructions.is_empty() {
                        continue;
                    }
                    match self.segments_for_object(obj, bank_size) {
                        Ok(segments) => layout.extend(segments.into_iter()
                            .map(|segment| (file_idx, segment))),
                        Err(e) => errors.push(format!("File {}: {}", file_idx, e)),
                    }
                }
                layout
            }
        };
        for (file_idx, segment) in layout {
            let obj = &object_files[file_idx];
            let len = segment.end - segment.start;
            if !packed_instructions.is_empty() {
                let used = packed_instructions.len() % bank_size;
                let remainder = if used == 0 { bank_size } else { bank_size - used };
                if len > remainder {
                    Self::pad_to_bank_end(&mut packed_instructions, bank_size);
                }
            }

            for local_idx in segment.start..segment.end {
                file_maps[file_idx][local_idx] = packed_instructions.len();
                packed_instructions.push(obj.instructions[local_idx]);
            }
        }

        // Concatenate data independently of code banking
        let mut all_data = Vec::new();
        // RCC's .space blocks reserve GP-bank addresses; they are initialized
        // by generated code, not by loading bytes into bank-zero legacy data.
        // Track this per object so mixing in a legacy .data object stays visible.
        let bf_host_gp_data_only = object_files.iter().all(|obj| {
            obj.data.is_empty() || (obj.data.iter().all(|&byte| byte == 0)
                && obj.data_labels.keys().any(|name| name.strip_prefix("__gp_")
                    .map(|tag| obj.labels.contains_key(&format!("__rcc_init_{tag}")))
                    .unwrap_or(false)))
        });
        let mut global_labels = HashMap::new();
        let mut global_data_labels = HashMap::new();
        let mut relocs: Vec<Reloc> = Vec::new();

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

        if all_data.len() > bank_size {
            errors.push(format!("global data needs {} cells, exceeds GP bank capacity {} (would overlap the stack bank)", all_data.len(), bank_size));
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
                } else if unresolved.ref_type == "absolute"
                    && resolved_instructions[global_idx].opcode == Opcode::Jal as u8
                {
                    relocs.push(Reloc { kind: RelocKind::JalAbs, index: global_idx as u32 });
                } else if unresolved.ref_type == "code_bank" {
                    relocs.push(Reloc { kind: RelocKind::JalAbs, index: global_idx as u32 });
                } else if unresolved.ref_type == "data"
                    && resolved_instructions[global_idx].opcode == Opcode::Li as u8
                {
                    relocs.push(Reloc { kind: RelocKind::LiData, index: global_idx as u32 });
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
                relocs.push(Reloc { kind: RelocKind::JalAbs, index: global_idx as u32 });
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
            bf_host_gp_data_only,
            labels: global_labels,
            data_labels: global_data_labels,
            entry_point,
            bank_size: self.bank_size,
            relocs,
        })
    }

    /// Compiler block labels are `L_{func}_{id}`. Everything else is treated as
    /// a function entry so a translation unit can be split without cutting a
    /// function (and its BEQ/BNE offsets) across a bank.
    fn is_function_entry_label(name: &str) -> bool {
        !name.starts_with("L_") && !name.starts_with('.')
    }

    fn nop_pad_instruction() -> Instruction {
        Instruction::new(Opcode::Nop, 0, 0, 0)
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

    fn returns_immediately(obj: &ObjectFile, label: &Label) -> bool {
        let index = label.absolute_address as usize / 4;
        !obj.unresolved_references.contains_key(&index)
            && obj.instructions.get(index).is_some_and(|inst|
                inst.opcode == Opcode::Jalr as u8
                    && inst.word1 == Register::R0 as u16
                    && inst.word2 == Register::Rab as u16
                    && inst.word3 == Register::Ra as u16)
    }

    fn cannot_fall_through(inst: &Instruction) -> bool {
        match Opcode::from_u8(inst.opcode) {
            Some(Opcode::Halt) => true,
            Some(Opcode::Jalr) => inst.word1 == Register::R0 as u16,
            Some(Opcode::Beq | Opcode::Bge) => inst.word1 == inst.word2,
            _ => false,
        }
    }

    fn has_opaque_jump(obj: &ObjectFile) -> bool {
        obj.instructions.iter().enumerate().any(|(index, inst)| {
            let reference = obj.unresolved_references.get(&index);
            match Opcode::from_u8(inst.opcode) {
                Some(Opcode::Jal) => reference.is_none()
                    && (inst.word2 != 0 || inst.word3 as usize >= obj.instructions.len()),
                Some(Opcode::Beq | Opcode::Bne | Opcode::Blt | Opcode::Bge) => {
                    let target = match reference {
                        Some(r) if r.ref_type == "branch" => obj.labels.get(&r.label)
                            .map(|label| label.absolute_address as i64 / 4),
                        Some(_) => None,
                        None => Some(index as i64 + i64::from(inst.word3 as i16)),
                    };
                    !target.is_some_and(|target| target >= 0 && target < obj.instructions.len() as i64)
                }
                _ => false,
            }
        })
    }

    /// A compiler metadata boundary alone does not authorize moving code: old
    /// or hand-written objects can still contain fallthrough and numeric jumps.
    /// Only separate functions whose relative branches remain inside them.
    fn movable_compiler_segments(obj: &ObjectFile) -> Option<Vec<CodeSegment>> {
        let meta = obj.rcc.as_ref()?;
        let mut starts: Vec<_> = meta.code.iter().map(|name|
            obj.labels.get(name).map(|label|
                (label.absolute_address as usize / 4, name.clone())))
            .collect::<Option<_>>()?;
        starts.sort();
        if starts.first().map(|s| s.0) != Some(0)
            || starts.windows(2).any(|pair| pair[0].0 == pair[1].0)
            || starts.last()?.0 >= obj.instructions.len()
        {
            return None;
        }
        let mut segments = Vec::new();
        for (i, (start, name)) in starts.iter().enumerate() {
            let end = starts.get(i + 1).map(|s| s.0).unwrap_or(obj.instructions.len());
            if !Self::cannot_fall_through(&obj.instructions[end - 1]) {
                return None;
            }
            for index in *start..end {
                let inst = &obj.instructions[index];
                let reference = obj.unresolved_references.get(&index);
                if matches!(Opcode::from_u8(inst.opcode),
                    Some(Opcode::Beq | Opcode::Bne | Opcode::Blt | Opcode::Bge))
                {
                    let target = match reference {
                        Some(r) if r.ref_type == "branch" =>
                            obj.labels.get(&r.label)?.absolute_address as i64 / 4,
                        Some(_) => return None,
                        None => index as i64 + i64::from(inst.word3 as i16),
                    };
                    if target < *start as i64 || target >= end as i64 {
                        return None;
                    }
                }
                if inst.opcode == Opcode::Jal as u8 && reference.is_none()
                    && (inst.word2 != 0 || inst.word3 as usize >= obj.instructions.len())
                {
                    return None;
                }
            }
            segments.push(CodeSegment { start: *start, end, name: Some(name.clone()) });
        }
        Some(segments)
    }

    /// Keep the entry object at its original beginning, then place the generated
    /// dispatcher and movable C initializers before ordinary functions. This
    /// keeps startup jumps short on a linear BF tape. Data and object order are
    /// untouched, so GP reservations and translation-unit offsets stay stable.
    fn startup_layout(
        &self,
        objects: &[ObjectFile],
        dispatcher: usize,
        bank_size: usize,
    ) -> Option<Vec<(usize, CodeSegment)>> {
        if !objects.iter().any(|obj| obj.entry_point.is_some()) {
            return None;
        }
        let first = objects.iter().position(|obj| !obj.instructions.is_empty())?;
        // Inserting code between legacy objects must not change fallthrough or
        // invalidate an absolute numeric target outside its defining object.
        if objects.iter().any(|obj| Self::has_opaque_jump(obj)
            || obj.instructions.last().is_some_and(|inst| !Self::cannot_fall_through(inst)))
        {
            return None;
        }
        let mut layout = Vec::new();
        let mut initializers = Vec::new();
        let mut ordinary = Vec::new();
        for (file, obj) in objects.iter().enumerate() {
            if obj.instructions.is_empty() {
                continue;
            }
            if file == first || file == dispatcher {
                let segments = self.segments_for_object(obj, bank_size).ok()?;
                layout.extend(segments.into_iter().map(|segment| (file, segment)));
            } else if let Some(segments) = Self::movable_compiler_segments(obj) {
                for segment in segments {
                    if segment.end - segment.start > bank_size {
                        return None; // ordinary packing reports the precise error
                    }
                    if segment.name.as_ref().is_some_and(|n| n.starts_with("__rcc_init_")) {
                        initializers.push((file, segment));
                    } else {
                        ordinary.push((file, segment));
                    }
                }
            } else {
                // An opaque object may have relative branches between
                // function boundaries. Changing where its separately packed
                // pieces cross banks would invalidate those branch offsets.
                if obj.instructions.len() > bank_size {
                    return None;
                }
                ordinary.extend(self.segments_for_object(obj, bank_size).ok()?
                    .into_iter().map(|segment| (file, segment)));
            }
        }
        layout.extend(initializers);
        layout.extend(ordinary);
        Some(layout)
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
            "code_bank" | "code_offset" => {
                let label = labels.get(&reference.label)
                    .ok_or_else(|| format!("Undefined function label: {}", reference.label))?;
                let index = label.absolute_address / 4;
                instruction.word2 = if reference.ref_type == "code_bank" {
                    (index / self.bank_size.max(1) as u32) as u16
                } else {
                    (index % self.bank_size.max(1) as u32) as u16
                };
            }
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

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
#[repr(u16)]
pub enum RelocKind {
    JalAbs = 0,
    LiData = 1,
    DataWord = 2,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Reloc {
    pub kind: RelocKind,
    pub index: u32,
}

#[derive(Debug)]
pub struct LinkedProgram {
    pub instructions: Vec<Instruction>,
    pub data: Vec<u8>,
    /// Every nonempty data object is an RCC zero-filled GP reservation.
    pub bf_host_gp_data_only: bool,
    pub labels: HashMap<String, Label>,
    pub data_labels: HashMap<String, u32>,
    pub entry_point: u32,
    pub bank_size: u16,
    pub relocs: Vec<Reloc>,
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

    pub fn entry_insn(&self) -> u32 {
        self.entry_point / 4
    }

    /// Add `base` to every JAL code-bank immediate so the image can live at PCB `base`.
    pub fn apply_code_bank_base(&mut self, base: u16) {
        if base == 0 {
            return;
        }
        for inst in &mut self.instructions {
            if inst.opcode == Opcode::Jal as u8 {
                inst.word2 = inst.word2.wrapping_add(base);
            }
        }
        // Code-bank LI operands use the existing word2/JalAbs relocation,
        // so existing image loaders need no new format or opcode.
        for reloc in &self.relocs {
            if reloc.kind == RelocKind::JalAbs {
                let inst = &mut self.instructions[reloc.index as usize];
                if inst.opcode != Opcode::Jal as u8 {
                    inst.word2 = inst.word2.wrapping_add(base);
                }
            }
        }
    }

    fn write_instructions(binary: &mut Vec<u8>, instructions: &[Instruction]) {
        for inst in instructions {
            binary.push(inst.opcode);
            binary.push(inst.word0);
            binary.extend_from_slice(&inst.word1.to_le_bytes());
            binary.extend_from_slice(&inst.word2.to_le_bytes());
            binary.extend_from_slice(&inst.word3.to_le_bytes());
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
        Self::write_instructions(&mut binary, &self.instructions);
        
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

    /// SYS1: on-disk boot module, already linked for `code_bank`.
    pub fn to_sys(&self, code_bank: u16, gp_bank: u16, sb_bank: u16) -> Vec<u8> {
        let mut binary = Vec::new();
        binary.extend_from_slice(b"SYS1");
        binary.extend_from_slice(&self.bank_size.to_le_bytes());
        binary.extend_from_slice(&code_bank.to_le_bytes());
        binary.extend_from_slice(&gp_bank.to_le_bytes());
        binary.extend_from_slice(&sb_bank.to_le_bytes());
        binary.extend_from_slice(&self.entry_insn().to_le_bytes());
        binary.extend_from_slice(&(self.instructions.len() as u32).to_le_bytes());
        binary.extend_from_slice(&(self.data.len() as u32).to_le_bytes());
        Self::write_instructions(&mut binary, &self.instructions);
        binary.extend_from_slice(&self.data);
        binary
    }

    /// RXE1: relocatable app. JAL banks are relative to image base 0.
    pub fn to_rxe(&self) -> Vec<u8> {
        let mut binary = Vec::new();
        binary.extend_from_slice(b"RXE1");
        binary.extend_from_slice(&self.bank_size.to_le_bytes());
        binary.extend_from_slice(&0u16.to_le_bytes());
        binary.extend_from_slice(&self.entry_insn().to_le_bytes());
        binary.extend_from_slice(&(self.instructions.len() as u32).to_le_bytes());
        binary.extend_from_slice(&(self.data.len() as u32).to_le_bytes());
        binary.extend_from_slice(&(self.relocs.len() as u32).to_le_bytes());
        Self::write_instructions(&mut binary, &self.instructions);
        binary.extend_from_slice(&self.data);
        for reloc in &self.relocs {
            binary.extend_from_slice(&(reloc.kind as u16).to_le_bytes());
            binary.extend_from_slice(&0u16.to_le_bytes());
            binary.extend_from_slice(&reloc.index.to_le_bytes());
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
    fn code_pointer_components_follow_packing_and_existing_bank_relocation() {
        let assembler = RippleAssembler::new(AssemblerOptions::default());
        let source = "start:\nLI T0, %offset(callback)\nLI T1, %bank(callback)\nHALT\n";
        let caller = assembler.assemble(source).unwrap();
        let pad = assembler.assemble(&format!("padding:\n{}", "NOP\n".repeat(16))).unwrap();
        let callback = assembler.assemble("callback:\nRET\n").unwrap();
        let mut linked = Linker::new(16).link(vec![caller, pad, callback]).unwrap();
        assert_eq!(linked.instructions[0].word2, 0);
        assert_eq!(linked.instructions[1].word2, 2);
        assert!(linked.relocs.iter().any(|r| r.index == 1 && r.kind == RelocKind::JalAbs));
        linked.apply_code_bank_base(7);
        assert_eq!(linked.instructions[0].word2, 0);
        assert_eq!(linked.instructions[1].word2, 9);
    }

    #[test]
    fn initializer_dispatch_uses_the_banked_stack_abi() {
        let assembler = RippleAssembler::new(AssemblerOptions::default());
        let start = assembler.assemble("start:\nCALL _init_globals\nHALT\n").unwrap();
        let init = assembler.assemble("__rcc_init_one:\nLI T0, 1\nRET\n").unwrap();
        let linked = Linker::new(16).link(vec![start, init]).unwrap();
        let index = (linked.labels["_init_globals"].absolute_address / 4) as usize;
        let save = linked.instructions[index];
        assert_eq!(save.opcode, Opcode::Store as u8);
        assert_eq!(save.word1, crate::types::Register::Ra as u16);
        assert_eq!(save.word2, crate::types::Register::Sb as u16);
        assert_eq!(save.word3, crate::types::Register::Sp as u16);
    }

    fn compiler_object(source: &str, code: &[&str]) -> ObjectFile {
        let mut options = AssemblerOptions::default();
        options.memory_offset = 0;
        let mut obj = RippleAssembler::new(options).assemble(source).unwrap();
        obj.rcc = Some(crate::types::CompilerMetadata {
            code: code.iter().map(|s| s.to_string()).collect(),
            // Keep every test function so this isolates layout from collection.
            roots: code.iter().map(|s| s.to_string()).collect(),
            ..Default::default()
        });
        obj
    }

    #[test]
    fn empty_initializers_keep_labels_and_gp_reservations_without_calls() {
        let assembler = RippleAssembler::new(AssemblerOptions::default());
        let start = assembler.assemble("start:\nCALL _init_globals\nHALT\n").unwrap();
        let init = compiler_object(
            ".data\n__gp_empty:\n.space 7\n.code\n__rcc_init_empty:\nRET\n",
            &["__rcc_init_empty"],
        );
        let linked = Linker::new(32).link(vec![start, init]).unwrap();
        let dispatcher = linked.labels["_init_globals"].absolute_address as usize / 4;
        assert_eq!(linked.instructions[dispatcher],
            Instruction::new(Opcode::Jalr, 0, Register::Rab as u16, Register::Ra as u16));
        assert_eq!(linked.instructions.iter().filter(|i| i.opcode == Opcode::Jal as u8).count(), 1);
        assert!(linked.labels.contains_key("__rcc_init_empty"));
        assert_eq!(linked.data_labels["__gp_empty"], 0);
        assert_eq!(linked.data.len(), 7);
        assert!(linked.bf_host_gp_data_only);
    }

    #[test]
    fn startup_layout_relocates_calls_pointers_and_data_across_banks() {
        let assembler = RippleAssembler::new(AssemblerOptions::default());
        let entry = assembler.assemble("start:\nCALL _init_globals\nCALL main\nHALT\n").unwrap();
        let padding = assembler.assemble(&format!("padding:\n{}RET\n", "NOP\n".repeat(31))).unwrap();
        let body = compiler_object(
            ".data\n__gp_tu:\n.space 5\n.code\n\
             __rcc_init_empty:\nRET\n\
             main:\nLI T0, %bank(helper)\nLI T1, %offset(helper)\nCALL helper\nRET\n\
             __rcc_init_tu:\nLI T0, __gp_tu\nLI T1, 7\nSTORE T1, GP, T0\nRET\n\
             helper:\nBEQ R0, R0, L_helper_skip\nNOP\nL_helper_skip:\nRET\n",
            &["__rcc_init_empty", "main", "__rcc_init_tu", "helper"],
        );
        let other = compiler_object(
            ".data\n__gp_other:\n.space 3\n.code\n__rcc_init_other:\nLI T0, __gp_other\nRET\n",
            &["__rcc_init_other"],
        );
        let mut linked = Linker::new(32).link(vec![entry, padding, body, other]).unwrap();
        let index = |name: &str| linked.labels[name].absolute_address as usize / 4;
        let dispatcher = index("_init_globals");
        let init = index("__rcc_init_tu");
        let other = index("__rcc_init_other");
        let main = index("main");
        let helper = index("helper");
        assert_eq!(dispatcher, 3);
        assert!(init < 32 && other < 32);
        assert!(main >= 64);
        assert_eq!((linked.instructions[0].word2, linked.instructions[0].word3), (0, 3));
        assert_eq!((linked.instructions[main + 2].word2, linked.instructions[main + 2].word3),
            ((helper / 32) as u16, (helper % 32) as u16));
        assert_eq!(linked.instructions[main].word2, (helper / 32) as u16);
        assert_eq!(linked.instructions[main + 1].word2, (helper % 32) as u16);
        assert_eq!(linked.instructions[helper].word3, 2);
        assert_eq!(linked.labels["L_helper_skip"].absolute_address / 4, (helper + 2) as u32);
        assert_eq!(linked.data_labels["__gp_tu"], 0);
        assert_eq!(linked.data_labels["__gp_other"], 5);
        assert_eq!(linked.instructions[init].word2, 0);
        assert_eq!(linked.instructions[other].word2, 5);
        assert!(linked.relocs.contains(&Reloc { kind: RelocKind::LiData, index: other as u32 }));
        // Preserve sorted initializer invocation order while omitting only RETs.
        assert_eq!(linked.instructions[dispatcher + 4].word3, other as u16);
        assert_eq!(linked.instructions[dispatcher + 5].word3, init as u16);
        linked.apply_code_bank_base(7);
        assert_eq!(linked.instructions[main].word2, (helper / 32 + 7) as u16);
        assert_eq!(linked.instructions[main + 1].word2, (helper % 32) as u16);
        assert_eq!(linked.instructions[main + 2].word2, (helper / 32 + 7) as u16);
    }

    #[test]
    fn startup_layout_preserves_opaque_or_fallthrough_compiler_objects() {
        let assembler = RippleAssembler::new(AssemblerOptions::default());
        for source in [
            "main:\nNOP\n__rcc_init_test:\nLI T0, 7\nRET\n",
            "main:\nBEQ R0, R0, __rcc_init_test\nRET\n__rcc_init_test:\nLI T0, 7\nRET\n",
            "main:\nJAL RA, 3, 100\nRET\n__rcc_init_test:\nLI T0, 7\nRET\n",
        ] {
            let entry = assembler.assemble("start:\nCALL _init_globals\nCALL main\nHALT\n").unwrap();
            let body = compiler_object(source, &["main", "__rcc_init_test"]);
            let original = body.instructions.clone();
            let linked = Linker::new(64).link(vec![entry, body]).unwrap();
            let main = linked.labels["main"].absolute_address as usize / 4;
            assert_eq!(&linked.instructions[main..main + original.len()], &original);
        }
    }

    #[test]
    fn startup_layout_does_not_insert_code_into_legacy_fallthrough() {
        let assembler = RippleAssembler::new(AssemblerOptions::default());
        let entry = assembler.assemble("start:\nCALL _init_globals\nNOP\n").unwrap();
        let body = compiler_object("main:\nHALT\n__rcc_init_test:\nLI T0, 7\nRET\n",
            &["main", "__rcc_init_test"]);
        let linked = Linker::new(32).link(vec![entry, body]).unwrap();
        assert_eq!(linked.labels["main"].absolute_address / 4, 2);
        assert_eq!(linked.instructions[2].opcode, Opcode::Halt as u8);
        assert!(linked.labels["_init_globals"].absolute_address > linked.labels["__rcc_init_test"].absolute_address);
    }

    #[test]
    fn startup_layout_keeps_bank_placement_of_opaque_oversized_compiler_objects() {
        let assembler = RippleAssembler::new(AssemblerOptions::default());
        let entry = assembler.assemble("start:\nCALL _init_globals\nCALL main\nHALT\n").unwrap();
        let body = compiler_object(
            &format!("main:\nBEQ R0, R0, __rcc_init_test\nRET\n\
                __rcc_init_test:\n{}RET\nhelper:\n{}RET\n",
                "NOP\n".repeat(4), "NOP\n".repeat(11)),
            &["main", "__rcc_init_test", "helper"],
        );
        let linked = Linker::new(16).link(vec![entry, body]).unwrap();
        // Moving the dispatcher before this object would put main at 13 and
        // its relative branch target at 16, on a different code bank.
        assert_eq!(linked.labels["main"].absolute_address / 4, 3);
        assert_eq!(linked.labels["__rcc_init_test"].absolute_address / 4, 5);
        assert_eq!(linked.instructions[3].word3, 2);
        assert_eq!(linked.labels["helper"].absolute_address / 4, 16);
    }

    #[test]
    fn custom_initializer_dispatcher_is_not_reordered_or_replaced() {
        let assembler = RippleAssembler::new(AssemblerOptions::default());
        let entry = assembler.assemble("start:\nCALL _init_globals\nHALT\n").unwrap();
        let body = compiler_object("main:\nRET\n__rcc_init_test:\nLI T0, 7\nRET\n",
            &["main", "__rcc_init_test"]);
        let custom = assembler.assemble("_init_globals:\nLI T1, 9\nRET\n").unwrap();
        let linked = Linker::new(32).link(vec![entry, body, custom]).unwrap();
        assert_eq!(linked.labels["main"].absolute_address / 4, 2);
        assert_eq!(linked.labels["_init_globals"].absolute_address / 4, 5);
        assert_eq!(linked.instructions[5], Instruction::new(Opcode::Li, Register::T1 as u16, 9, 0));
    }

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

    #[test]
    fn test_local_call_beyond_16_bit_object_offset() {
        let mut options = AssemblerOptions::default();
        options.bank_size = 64000;
        options.max_immediate = 65535;
        let assembler = RippleAssembler::new(options);
        let prefix = assembler.assemble("prefix:\nNOP\nNOP\nNOP\n").unwrap();
        let mut source = String::from("start:\nCALL late\n");
        source.push_str(&"NOP\n".repeat(39999));
        source.push_str("middle:\n");
        source.push_str(&"NOP\n".repeat(30000));
        source.push_str("late:\nCALL start\nRET\n");
        let object = assembler.assemble(&source).unwrap();
        assert_eq!(object.labels["late"].absolute_address / 4, 70000);
        assert_eq!(object.unresolved_references[&0].label, "late");
        assert_eq!(object.unresolved_references[&0].ref_type, "absolute");
        assert!(assembler.assemble_to_binary(&source).unwrap_err()[0].contains("rlink"));

        let mut linked = Linker::new(64000).link(vec![prefix, object]).unwrap();
        let start = (linked.labels["start"].absolute_address / 4) as usize;
        let late = (linked.labels["late"].absolute_address / 4) as usize;
        assert_eq!(start, 3);
        assert_eq!(late, 94000);
        let forward = &linked.instructions[start];
        assert_eq!((forward.word2, forward.word3), (1, 30000));
        let backward = &linked.instructions[late];
        assert_eq!((backward.word2, backward.word3), (0, 3));
        linked.apply_code_bank_base(7);
        assert_eq!(linked.instructions[start].word2, 8);
        assert_eq!(linked.instructions[late].word2, 7);
    }

    #[test]
    fn test_link_relocates_data_labels_across_objects() {
        let mut options = AssemblerOptions::default();
        options.memory_offset = 0;
        let assembler = RippleAssembler::new(options);

        let src1 = r#"
.data
base1:
    .space 10
.code
start:
    LI R3, base1
    HALT
"#;
        let src2 = r#"
.data
base2:
    .space 5
.code
func:
    LI R4, base2
    RET
"#;
        let obj1 = assembler.assemble(src1).unwrap();
        let obj2 = assembler.assemble(src2).unwrap();
        assert_eq!(obj1.data.len(), 10);
        assert_eq!(obj2.data.len(), 5);

        let linker = Linker::new(16);
        let linked = linker.link(vec![obj1, obj2]).unwrap();

        assert_eq!(linked.data.len(), 15);
        assert_eq!(linked.data_labels.get("base1"), Some(&0));
        assert_eq!(linked.data_labels.get("base2"), Some(&10));

        // start: LI R3, base1
        assert_eq!(linked.instructions[0].opcode, Opcode::Li as u8);
        assert_eq!(linked.instructions[0].word2, 0);
        // func: LI R4, base2 (after start + HALT)
        assert_eq!(linked.instructions[2].opcode, Opcode::Li as u8);
        assert_eq!(linked.instructions[2].word2, 10);
    }

    #[test]
    fn test_sys_and_rxe_headers() {
        let assembler = RippleAssembler::new(AssemblerOptions::default());
        let obj = assembler.assemble("start:\n    CALL dest\n    HALT\ndest:\n    RET\n").unwrap();
        let linker = Linker::new(16);
        let mut linked = linker.link(vec![obj]).unwrap();
        assert!(linked.relocs.iter().any(|r| r.kind == RelocKind::JalAbs));

        let rxe = linked.to_rxe();
        assert_eq!(&rxe[0..4], b"RXE1");

        linked.apply_code_bank_base(1);
        let sys = linked.to_sys(1, 3, 2);
        assert_eq!(&sys[0..4], b"SYS1");
        assert_eq!(u16::from_le_bytes([sys[6], sys[7]]), 1);
        // First instruction is JAL; bank should be 1 after apply_code_bank_base
        assert_eq!(linked.instructions[0].opcode, Opcode::Jal as u8);
        assert_eq!(linked.instructions[0].word2, 1);
    }
}
