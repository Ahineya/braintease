use crate::types::{Instruction, Opcode};
use crate::linker::LinkedProgram;
use std::collections::HashMap;

const CPU_HEADER: &str = include_str!("cpu_header.bfm");
pub(crate) const CPU_FOOTER: &str = include_str!("cpu_footer.bfm");

pub struct MacroFormatter {
    irq: bool,
    code_banks: u16,
    opcode_to_macro: HashMap<u8, &'static str>,
}

impl MacroFormatter {
    pub fn new() -> Self {
        let mut opcode_to_macro = HashMap::new();
        
        opcode_to_macro.insert(Opcode::Halt as u8, "@OP_HALT");
        opcode_to_macro.insert(Opcode::Nop as u8, "@OP_NOP");
        opcode_to_macro.insert(Opcode::Add as u8, "@OP_ADD");
        opcode_to_macro.insert(Opcode::Sub as u8, "@OP_SUB");
        opcode_to_macro.insert(Opcode::And as u8, "@OP_AND");
        opcode_to_macro.insert(Opcode::Or as u8, "@OP_OR");
        opcode_to_macro.insert(Opcode::Xor as u8, "@OP_XOR");
        opcode_to_macro.insert(Opcode::Sll as u8, "@OP_SL");
        opcode_to_macro.insert(Opcode::Srl as u8, "@OP_SR");
        opcode_to_macro.insert(Opcode::Slt as u8, "@OP_SLT");
        opcode_to_macro.insert(Opcode::Sltu as u8, "@OP_SLTU");
        opcode_to_macro.insert(Opcode::Addi as u8, "@OP_ADDI");
        opcode_to_macro.insert(Opcode::Andi as u8, "@OP_ANDI");
        opcode_to_macro.insert(Opcode::Ori as u8, "@OP_ORI");
        opcode_to_macro.insert(Opcode::Xori as u8, "@OP_XORI");
        opcode_to_macro.insert(Opcode::Li as u8, "@OP_LI");
        opcode_to_macro.insert(Opcode::Slli as u8, "@OP_SLLI");
        opcode_to_macro.insert(Opcode::Srli as u8, "@OP_SRLI");
        opcode_to_macro.insert(Opcode::Load as u8, "@OP_LOAD");
        opcode_to_macro.insert(Opcode::Store as u8, "@OP_STOR");
        opcode_to_macro.insert(Opcode::Jal as u8, "@OP_JAL");
        opcode_to_macro.insert(Opcode::Jalr as u8, "@OP_JALR");
        opcode_to_macro.insert(Opcode::Beq as u8, "@OP_BEQ");
        opcode_to_macro.insert(Opcode::Bne as u8, "@OP_BNE");
        opcode_to_macro.insert(Opcode::Blt as u8, "@OP_BLT");
        opcode_to_macro.insert(Opcode::Bge as u8, "@OP_BGE");
        opcode_to_macro.insert(Opcode::Brk as u8, "@OP_BRK");
        opcode_to_macro.insert(Opcode::Mul as u8, "@OP_MUL");
        opcode_to_macro.insert(Opcode::Div as u8, "@OP_DIV");
        opcode_to_macro.insert(Opcode::Mod as u8, "@OP_MOD");
        opcode_to_macro.insert(Opcode::Muli as u8, "@OP_MULI");
        opcode_to_macro.insert(Opcode::Divi as u8, "@OP_DIVI");
        opcode_to_macro.insert(Opcode::Modi as u8, "@OP_MODI");
        opcode_to_macro.insert(Opcode::Loadc as u8, "@OP_LOADC");
        opcode_to_macro.insert(Opcode::Storc as u8, "@OP_STORC");
        
        opcode_to_macro.insert(Opcode::Iret as u8, "@OP_IRET");
        opcode_to_macro.insert(Opcode::Csrr as u8, "@OP_CSRR");
        opcode_to_macro.insert(Opcode::Csrw as u8, "@OP_CSRW");
        Self { opcode_to_macro, irq: false, code_banks: 0 }
    }

    /// Interrupt microcode is opt-in; disabled images have no boundary polling.
    pub fn with_irq(mut self, enabled: bool) -> Self { self.irq = enabled; self }

    /// Reserve Harvard code space for programs loaded with STORC after boot.
    pub fn with_code_banks(mut self, banks: u16) -> Self { self.code_banks = banks; self }

    fn register_to_macro(&self, reg: u16) -> String {
        // The existing BF CPU has the same 32 physical registers as the ISA.
        // In particular R5/R6 are RV0/RV1, not the old shifted register names.
        format!("#R{reg}")
    }

    pub fn format_instruction(&self, instruction: &Instruction, is_first: bool) -> String {
        let mut opcode_macro = self.opcode_to_macro
            .get(&instruction.opcode)
            .copied()
            .unwrap_or_else(|| {
                panic!("Unknown opcode {} (0x{:02x}) in macro formatter", 
                       instruction.opcode, instruction.opcode)
            });
        
        // HALT is its own opcode; keep the lookup for unknown 0x00 encodings
        if instruction.is_halt() {
            opcode_macro = "@OP_HALT";
        }
        
        // Special case for BRK (encoded as BRK 0,0,0)
        if instruction.opcode == Opcode::Brk as u8 && 
            instruction.word1 == 0 && 
            instruction.word2 == 0 && 
            instruction.word3 == 0 {
            opcode_macro = "@OP_BRK";
        }

        let opcode = instruction.opcode;
        let format_operand = |value: u16, is_register: bool| -> String {
            if is_register {
                self.register_to_macro(value)
            } else {
                value.to_string()
            }
        };

        // Determine which operands are registers based on opcode
        let is_r_format = matches!(opcode,
            x if x == Opcode::Add as u8 || x == Opcode::Sub as u8 || 
                 x == Opcode::And as u8 || x == Opcode::Or as u8 || 
                 x == Opcode::Xor as u8 || x == Opcode::Sll as u8 || 
                 x == Opcode::Srl as u8 || x == Opcode::Slt as u8 || 
                 x == Opcode::Sltu as u8 || x == Opcode::Jalr as u8 ||
                 x == Opcode::Mul as u8 || x == Opcode::Div as u8 ||
                 x == Opcode::Mod as u8 || x == Opcode::Loadc as u8 ||
                 x == Opcode::Storc as u8
        );

        let is_i_format = matches!(opcode,
            x if x == Opcode::Addi as u8 || x == Opcode::Andi as u8 || 
                 x == Opcode::Ori as u8 || x == Opcode::Xori as u8 ||
                 x == Opcode::Slli as u8 || x == Opcode::Srli as u8 || 
                 x == Opcode::Load as u8 || x == Opcode::Store as u8 ||
                 x == Opcode::Beq as u8 || x == Opcode::Bne as u8 || 
                 x == Opcode::Blt as u8 || x == Opcode::Bge as u8 ||
                 x == Opcode::Muli as u8 || x == Opcode::Divi as u8 ||
                 x == Opcode::Modi as u8
        );

        let (word1_str, word2_str, word3_str) = if is_r_format {
            (
                format_operand(instruction.word1, true),
                format_operand(instruction.word2, true),
                format_operand(instruction.word3, true),
            )
        } else if is_i_format {
            // Special case for LOAD: all positions can be registers
            if instruction.opcode == Opcode::Load as u8 {
                (
                    format_operand(instruction.word1, true),
                    format_operand(instruction.word2, true),
                    format_operand(instruction.word3, true),
                )
            } else {
                (
                    format_operand(instruction.word1, true),
                    format_operand(instruction.word2, true),
                    format_operand(instruction.word3, false),
                )
            }
        } else if instruction.opcode == Opcode::Li as u8 {
            (
                format_operand(instruction.word1, true),
                format_operand(instruction.word2, false),
                format_operand(instruction.word3, false),
            )
        } else if instruction.opcode == Opcode::Jal as u8 {
            (
                format_operand(instruction.word1, false),
                format_operand(instruction.word2, false),
                format_operand(instruction.word3, false),
            )
        } else {
            (
                format_operand(instruction.word1, false),
                format_operand(instruction.word2, false),
                format_operand(instruction.word3, false),
            )
        };

        let cmd_type = if is_first { "@program_start" } else { "@cmd" };
        
        format!(
            "{}({:10}, {:4}, {:4}, {})",
            cmd_type,
            opcode_macro,
            word1_str,
            word2_str,
            word3_str
        )
    }

    pub fn format_program(&self, instructions: &[Instruction], comments: Option<&HashMap<usize, String>>) -> String {
        self.format_program_at_entry(instructions, comments, 0)
    }

    fn format_program_at_entry(&self, instructions: &[Instruction], comments: Option<&HashMap<usize, String>>, entry: usize) -> String {
        let mut lines = Vec::new();
        
        for (index, instruction) in instructions.iter().enumerate() {
            let comment = comments.and_then(|c| c.get(&index));
            let mut formatted_inst = self.format_instruction(instruction, index == 0);
            if entry != 0 {
                let command = if index == 0 { "@program_prefix_start" }
                    else if index < entry { "@cmd_prefix" }
                    else if index == entry { "@cmd_entry" }
                    else { "@cmd" };
                let arguments = &formatted_inst[formatted_inst.find('(').unwrap()..];
                formatted_inst = format!("{command}{arguments}");
            }
            
            if let Some(comment_text) = comment {
                lines.push(format!("{}        // {}", formatted_inst, comment_text));
            } else {
                lines.push(formatted_inst);
            }
        }
        
        lines.push("@program_end".to_string());
        
        lines.join("\n")
    }

    pub fn format_data_section(&self, data: &[u8]) -> String {
        let mut lines = Vec::new();
        
        lines.push("// Data segment".to_string());
        lines.push("@lane(#L_MEM,".to_string());
        
        // Convert data array to mixed format array (decimal, hex, or char)
        let formatted_data: Vec<String> = data.iter().map(|&value| {
            match value {
                // Special characters - use decimal
                9 => "9".to_string(),    // tab
                10 => "10".to_string(),  // newline
                13 => "13".to_string(),  // carriage return
                // Printable ASCII (space through ~)
                32..=126 => {
                    let ch = value as char;
                    // Escape single quotes and backslashes
                    match ch {
                        '\'' => "'\\'".to_string(),
                        '\\' => "'\\\\'".to_string(),
                        _ => format!("'{}'", ch),
                    }
                }
                // Use hex for high values
                128..=255 => format!("0x{:02X}", value),
                // Use decimal for everything else
                _ => value.to_string(),
            }
        }).collect();
        
        // Create the {for} loop with formatted data
        let data_str = formatted_data.join(",");
        lines.push(format!("  {{for(s in {{{}}}, @set(s) @nextword)}}", data_str));
        
        lines.push(")".to_string());
        
        lines.join("\n")
    }

    pub fn format_full_program(
        &self,
        instructions: &[Instruction],
        data: Option<&[u8]>,
        comments: Option<&HashMap<usize, String>>,
        header: Option<&str>,
    ) -> String {
        self.format_full_program_at_entry(instructions, data, comments, header, 0)
    }

    fn format_full_program_at_entry(
        &self,
        instructions: &[Instruction],
        data: Option<&[u8]>,
        comments: Option<&HashMap<usize, String>>,
        header: Option<&str>,
        entry: usize,
    ) -> String {
        let mut lines = Vec::new();
        
        if let Some(header_text) = header {
            lines.push(format!("// {}", header_text));
            lines.push(String::new());
        }
        
        lines.push("@prg(".to_string());
        
        // Format data section or @nop if empty
        if let Some(data_bytes) = data {
            if !data_bytes.is_empty() {
                lines.push("  // Memory".to_string());
                let data_lines = self.format_data_section(data_bytes);
                for (idx, line) in data_lines.lines().enumerate() {
                    if idx == 0 && line.starts_with("// Data segment") {
                        // Skip the old header comment
                        continue;
                    }
                    if line == ")" {
                        // Change the closing paren to include comma
                        lines.push("  ),".to_string());
                    } else {
                        lines.push(format!("  {}", line));
                    }
                }
                lines.push("  ".to_string());
            } else {
                lines.push("  @nop,".to_string());
                lines.push("  ".to_string());
            }
        } else {
            lines.push("  @nop,".to_string());
            lines.push("  ".to_string());
        }
        
        // Format program section or @nop if empty
        if !instructions.is_empty() {
            lines.push("  // Program".to_string());
            let program_lines = self.format_program_at_entry(instructions, comments, entry);
            for line in program_lines.lines() {
                lines.push(format!("  {}", line));
            }
        } else {
            lines.push("  @nop".to_string());
        }
        
        lines.push(")".to_string());
        
        lines.join("\n")
    }

    pub fn format_linked_program(&self, program: &LinkedProgram) -> String {
        let comments = HashMap::new(); // Could be enhanced to preserve comments
        self.format_full_program(
            &program.instructions,
            Some(&program.data),
            Some(&comments),
            Some(&format!("Linked program with entry point at 0x{:08X}", program.entry_point)),
        )
    }

    pub fn format_linked_program_standalone(&self, program: &LinkedProgram, debug: bool) -> String {
        self.format_standalone(program, debug, None)
    }

    /// An instrumented BF CPU remains ordinary Brainfuck computation, with
    /// explicit host callbacks at completed memory accesses. Its ASCII header
    /// deliberately contains no BF commands (or host callback characters).
    pub fn format_linked_program_host(&self, program: &LinkedProgram, memory_banks: u16) -> Result<String, String> {
        self.validate_linked_program_host(program, memory_banks)?;
        Ok(self.format_standalone(program, true, Some(memory_banks)))
    }

    pub(crate) fn validate_linked_program_host(&self, program: &LinkedProgram, memory_banks: u16) -> Result<(), String> {
        if program.bank_size < 32 {
            return Err("BF host bank size must be at least 32 cells for MMIO".into());
        }
        if memory_banks == 0 {
            return Err("BF host requires at least one RAM bank".into());
        }
        if !program.bf_host_gp_data_only {
            return Err("BF host does not support legacy .data overlapping MMIO; use C global initializers".into());
        }
        if program.instructions.len() >= usize::from(program.bank_size) * 65536 {
            return Err("BF code image and final HALT must fit the 16-bit PC/PCB address space".into());
        }
        if program.entry_point % 4 != 0 || program.entry_point as usize / 4 >= program.instructions.len() {
            return Err("BF host entry point is outside the instruction image".into());
        }
        for instruction in &program.instructions {
            if matches!(instruction.opcode, 0x25 | 0x26) ||
                (matches!(instruction.opcode,0x23 | 0x24) && instruction.word2 >= 12) {
                return Err("MMU instructions require RVM; build the Brainfuck profile with MMU=0".into());
            }
            if !self.irq && matches!(instruction.opcode, 0x22..=0x24) {
                return Err("BF IRQ instructions require --bf-irq (ENABLE_IRQ=1)".into());
            }
            if !self.opcode_to_macro.contains_key(&instruction.opcode) {
                return Err(format!("Unknown opcode {} in BF image", instruction.opcode));
            }
            // Macro exports used #Rn names for register operands; malformed
            // indices failed during expansion. Direct BF must reject them too.
            let registers: &[u16] = match Opcode::from_u8(instruction.opcode).unwrap() {
                Opcode::Halt | Opcode::Nop | Opcode::Brk | Opcode::Iret => &[],
                Opcode::Csrr | Opcode::Csrw => std::slice::from_ref(&instruction.word1),
                Opcode::Li | Opcode::Jal => std::slice::from_ref(&instruction.word1),
                Opcode::Addi | Opcode::Andi | Opcode::Ori | Opcode::Xori
                | Opcode::Slli | Opcode::Srli | Opcode::Muli | Opcode::Divi | Opcode::Modi
                | Opcode::Beq | Opcode::Bne | Opcode::Blt | Opcode::Bge => {
                    if instruction.word1 >= 32 || instruction.word2 >= 32 {
                        return Err("BF image contains an invalid register index".into());
                    }
                    &[]
                }
                _ => {
                    if instruction.word1 >= 32 || instruction.word2 >= 32 || instruction.word3 >= 32 {
                        return Err("BF image contains an invalid register index".into());
                    }
                    &[]
                }
            };
            if registers.iter().any(|&register| register >= 32) {
                return Err("BF image contains an invalid register index".into());
            }
        }
        Ok(())
    }

    pub(crate) fn standalone_prefix(&self, program: &LinkedProgram, debug: bool, memory_banks: Option<u16>) -> String {
        let mut output = String::new();
        let entry = program.entry_point as usize / 4;
        if let Some(banks) = memory_banks {
            // Include the implicit HALT and the ALU's temporary words beyond
            // the last instruction, plus a guard for lane seeking.
            let words = (usize::from(banks) * usize::from(program.bank_size))
                .max(76 + 4 * usize::from(self.code_banks) * usize::from(program.bank_size))
                .max(40 + 4 * program.instructions.len() + 36).max(crate::bf_cpu::MIN_WORDS) + 32;
            output.push_str(&format!("{{preserve(BFHOST 1 BANK_SIZE {} MEMORY_BANKS {} INSTRUCTIONS {} ENTRY {} ORIGIN 8 TAPE_CELLS {} END_BFHOST)}}\n>>>>>>>>\n",
                program.bank_size, banks, program.instructions.len(), entry, (words + 1) * 8));
        }
        
        // Add CPU header with modified DEBUG value
        let mut header = if debug {
            CPU_HEADER.replace("#define DEBUG 0", "#define DEBUG 1")
        } else {
            CPU_HEADER.to_string()
        };
        let bank_size = usize::from(program.bank_size.max(1));
        header = header.replace("#define ENTRY_POINT 0", &format!("#define ENTRY_POINT {}", entry % bank_size));
        header = header.replace("#define ENTRY_BANK 0", &format!("#define ENTRY_BANK {}", entry / bank_size));
        header = header.replace("#define PROGRAM_WORDS 0", &format!("#define PROGRAM_WORDS {}", 40 + 4 * program.instructions.len()));
        if memory_banks.is_some() {
            header = header.replace("#define HOST_MODE 0", "#define HOST_MODE 1")
                .replace("#define HOST_LOAD @nop", "#define HOST_LOAD {preserve(?)}")
                .replace("#define HOST_STORE @nop", "#define HOST_STORE {preserve(!)}")
                .replace("#define HOST_TTY @print", "#define HOST_TTY @nop")
                .replace("#define HOST_BREAK $", "#define HOST_BREAK @nop");
        }
        header = header.replace("#define ENABLE_IRQ 0", &format!("#define ENABLE_IRQ {}", u8::from(self.irq)));
        let definitions = crate::bf_cpu::definitions(program.bank_size, memory_banks, program.instructions.len(), self.code_banks);
        header = header.replace("#define PROGRAM {", &format!("{definitions}\n#define PROGRAM {{"));
        output.push_str(&header);
        output.push('\n');
        output
    }

    fn format_standalone(&self, program: &LinkedProgram, debug: bool, memory_banks: Option<u16>) -> String {
        let mut output = self.standalone_prefix(program, debug, memory_banks);
        let entry = program.entry_point as usize / 4;
        
        // Add the program content (without @prg wrapper since it's in the template)
        let comments = HashMap::new();
        let program_content = self.format_full_program_at_entry(
            &program.instructions,
            if memory_banks.is_some() { None } else { Some(&program.data) },
            Some(&comments),
            Some(&format!("Linked program with entry point at 0x{:08X}", program.entry_point)),
            entry,
        );
        
        // Add indentation to match the template structure
        for line in program_content.lines() {
            output.push_str("  ");
            output.push_str(line);
            output.push('\n');
        }
        
        // Add CPU footer
        output.push_str(&CPU_FOOTER.replace("#define BANK_SIZE 64000", &format!("#define BANK_SIZE {}", program.bank_size)));
        
        output
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::types::Register;

    #[test]
    fn test_format_halt() {
        let formatter = MacroFormatter::new();
        let inst = Instruction::new(Opcode::Halt, 0, 0, 0);
        let formatted = formatter.format_instruction(&inst, true);
        
        assert!(formatted.contains("@OP_HALT"));
        assert!(formatted.starts_with("@program_start"));
    }

    #[test]
    fn test_format_add() {
        let formatter = MacroFormatter::new();
        let inst = Instruction::new(Opcode::Add, Register::Rv0 as u16, Register::Rv1 as u16, Register::A0 as u16);
        let formatted = formatter.format_instruction(&inst, false);
        
        assert!(formatted.contains("@OP_ADD"));
        assert!(formatted.contains("#R5"));
        assert!(formatted.contains("#R6"));
        assert!(formatted.contains("#R7"));
        assert!(formatted.starts_with("@cmd"));
    }

    #[test]
    fn physical_register_macros_match_existing_bf_cpu() {
        let formatter = MacroFormatter::new();
        let definitions: HashMap<_, _> = CPU_FOOTER.lines().filter_map(|line| {
            let mut words = line.split_whitespace();
            if words.next()? != "#define" { return None; }
            Some((words.next()?, words.next()?))
        }).collect();
        for reg in 0..32 {
            let name = formatter.register_to_macro(reg);
            let mut value = name.as_str();
            while let Some(symbol) = value.strip_prefix('#') {
                value = definitions[symbol];
            }
            assert_eq!(value.parse::<u16>().unwrap(), reg);
        }
    }

    #[test]
    fn shift_opcodes_use_existing_bf_cpu_macros() {
        let formatter = MacroFormatter::new();
        for opcode in [Opcode::Sll, Opcode::Srl] {
            let name = formatter.opcode_to_macro[&(opcode as u8)].trim_start_matches('@');
            assert!(CPU_FOOTER.contains(&format!("#define {name} ")));
        }
    }

    #[test]
    fn test_format_data_section() {
        let formatter = MacroFormatter::new();
        let data = b"Hello\x00\x01\xFF";
        let formatted = formatter.format_data_section(data);
        
        assert!(formatted.contains("'H'"));
        assert!(formatted.contains("'e'"));
        assert!(formatted.contains("'l'"));
        assert!(formatted.contains("'o'"));
        assert!(formatted.contains("0"));  // null byte
        assert!(formatted.contains("1"));  // 0x01
        assert!(formatted.contains("0xFF"));  // 0xFF
    }

    #[test]
    fn test_format_full_program() {
        let formatter = MacroFormatter::new();
        let instructions = vec![
            Instruction::new(Opcode::Li, Register::Ra as u16, 42, 0),
            Instruction::new(Opcode::Halt, 0, 0, 0),
        ];
        let data = b"Test";
        
        let formatted = formatter.format_full_program(
            &instructions,
            Some(data),
            None,
            Some("Test Program"),
        );
        
        assert!(formatted.contains("// Test Program"));
        assert!(formatted.contains("@prg("));
        assert!(formatted.contains("@OP_LI"));
        assert!(formatted.contains("@OP_HALT"));
        assert!(formatted.contains("'T'"));
        assert!(formatted.contains("'e'"));
        assert!(formatted.contains("'s'"));
        assert!(formatted.contains("'t'"));
    }
}
