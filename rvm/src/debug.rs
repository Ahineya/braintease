use colored::*;
use ripple_asm::Register;
use crate::vm::VM;

pub struct Debugger {}

impl Default for Debugger {
    fn default() -> Self {
        Self::new()
    }
}

impl Debugger {
    pub fn new() -> Self {
        Self {}
    }
    
    /// Format an instruction for display
    pub fn format_instruction(&self, vm: &VM) -> Option<String> {
        let instr = vm.get_current_instruction()?;
        
        // Create a simple disassembly of the instruction
        let opcode_str = Self::opcode_name(instr.opcode);
        
        // Format based on instruction type
        let formatted = match instr.opcode {
            0x00 => {
                "HALT".to_string()
            },
            0x42 => {
                "NOP".to_string()
            },
            0x01..=0x09 | 0x1A..=0x1C | 0x20 | 0x21 => {
                // R-format: ADD, SUB, AND, OR, XOR, SLL, SRL, SLT, SLTU, MUL, DIV, MOD, LOADC, STORC
                let rd = Self::register_name(instr.word1 as u8);
                let rs = Self::register_name(instr.word2 as u8);
                let rt = Self::register_name(instr.word3 as u8);
                format!("{opcode_str} {rd}, {rs}, {rt}")
            },
            0x0A..=0x0D | 0x0F | 0x10 | 0x1D..=0x1F => {
                // I-format with immediate: ADDI, ANDI, ORI, XORI, SLLI, SRLI, MULI, DIVI, MODI
                let rd = Self::register_name(instr.word1 as u8);
                let rs = Self::register_name(instr.word2 as u8);
                let imm = instr.word3;
                format!("{} {}, {}, {}", opcode_str, rd, rs, Self::format_immediate(imm))
            },
            0x0E => {
                // LI - Load immediate
                let rd = Self::register_name(instr.word1 as u8);
                let imm = instr.word2;
                format!("{} {}, {}", opcode_str, rd, Self::format_immediate(imm))
            },
            0x11 => {
                // LOAD
                let rd = Self::register_name(instr.word1 as u8);
                let bank = Self::format_operand(instr.word2);
                let addr = Self::format_operand(instr.word3);
                format!("{opcode_str} {rd}, {bank}, {addr}")
            },
            0x12 => {
                // STORE
                let rs = Self::register_name(instr.word1 as u8);
                let bank = Self::format_operand(instr.word2);
                let addr = Self::format_operand(instr.word3);
                format!("{opcode_str} {rs}, {bank}, {addr}")
            },
            0x13 => {
                // JAL
                let rd = Self::register_name(instr.word1 as u8);
                let addr = instr.word3;
                format!("{opcode_str} {rd}, 0x{addr:04X}")
            },
            0x14 => {
                // JALR
                let rd = Self::register_name(instr.word1 as u8);
                let rs = Self::register_name(instr.word3 as u8);
                format!("{opcode_str} {rd}, {rs}")
            },
            0x15..=0x18 => {
                // Branch instructions: BEQ, BNE, BLT, BGE
                let rs = Self::register_name(instr.word1 as u8);
                let rt = Self::register_name(instr.word2 as u8);
                let offset = instr.word3 as i16;
                format!("{opcode_str} {rs}, {rt}, {offset}")
            },
            0x19 => {
                // BRK
                "BRK".to_string()
            },
            _ => {
                // Unknown instruction
                format!("UNKNOWN 0x{:02X}", instr.opcode)
            }
        };
        
        Some(formatted)
    }
    
    /// Print the current VM state in a pretty format
    pub fn print_state(&self, vm: &VM) {
        let pc = vm.registers[Register::Pc as usize];
        let pcb = vm.registers[Register::Pcb as usize];
        
        // Header
        println!("\n{}", "─".repeat(80).bright_black());
        
        // PC and state
        print!("{}: ", "PC".bright_cyan().bold());
        println!("{:04X}:{:04X}  {}: {}", 
            pcb, pc,
            "State".bright_cyan().bold(),
            Self::format_state(&vm.state)
        );
        
        // Registers in a nice grid
        println!("\n{}", "Registers:".bright_cyan().bold());
        
        // Special registers first
        print!("  {} ", format!("R0={:04X}", vm.registers[0]).bright_black());
        print!("  {} ", format!("PC={pc:04X}").bright_green());
        print!("  {} ", format!("PCB={pcb:04X}").bright_green());
        print!("  {} ", format!("RA={:04X}", vm.registers[Register::Ra as usize]).bright_magenta());
        println!("  {} ", format!("RAB={:04X}", vm.registers[Register::Rab as usize]).bright_magenta());
        
        // General purpose registers in rows of 5
        for row in 0..3 {
            print!(" ");
            for col in 0..5 {
                let reg_idx = 5 + row * 5 + col;
                if reg_idx <= 17 {
                    let value = vm.registers[reg_idx];
                    let formatted = format!("R{}={:04X}", reg_idx - 2, value);
                    
                    // Color non-zero values
                    if value != 0 {
                        print!("  {}", formatted.bright_white());
                    } else {
                        print!("  {}", formatted.bright_black());
                    }
                }
            }
            println!();
        }
        
        // Current instruction
        if let Some(formatted) = self.format_instruction(vm) {
            println!("\n{}", "Next Instruction:".bright_cyan().bold());
            
            print!("  [{pc:04X}] ");
            
            // Color the instruction based on type
            let instr = vm.get_current_instruction().unwrap();
            let colored = if instr.opcode == 0x19 { // BRK
                formatted.bright_red().bold()
            } else if instr.opcode == 0x00 { // HALT
                formatted.bright_red()
            } else if instr.opcode >= 0x13 && instr.opcode <= 0x18 { // Jumps/branches
                formatted.bright_yellow()
            } else if instr.opcode == 0x11 || instr.opcode == 0x12 { // Load/Store
                formatted.bright_blue()
            } else {
                formatted.normal()
            };
            
            println!("{colored}");
            
            // Show raw bytes in dim
            println!("       {}", 
                format!("[{:02X} {:02X} {:04X} {:04X} {:04X}]", 
                    instr.opcode, instr.word0, instr.word1, instr.word2, instr.word3
                ).bright_black()
            );
        }
        
        println!("{}", "─".repeat(80).bright_black());
    }
    
    /// Print a welcome message for debug mode
    pub fn print_welcome() {
        println!("{}", "╔══════════════════════════════════════════════════════════════╗".bright_cyan());
        println!("{}", "║         Ripple VM Debugger - Interactive Mode               ║".bright_cyan());
        println!("{}", "╚══════════════════════════════════════════════════════════════╝".bright_cyan());
        println!();
        println!("Commands:");
        println!("  {}  Step one instruction", "Enter".bright_green().bold());
        println!("  {}       Run to completion", "r".bright_green().bold());
        println!("  {}       Continue from breakpoint.rs", "c".bright_green().bold());
        println!("  {}       Quit debugger_ui", "q".bright_green().bold());
        println!();
        println!("Note: To restart after HALT, quit (q) and run again.");
        println!("      Or use TUI mode (-t) which supports restart with 'R'.");
        println!();
    }
    
    fn format_state(state: &crate::vm::VMState) -> ColoredString {
        match state {
            crate::vm::VMState::Setup => "Setup".bright_black(),
            crate::vm::VMState::Running => "Running".bright_green(),
            crate::vm::VMState::Halted => "Halted".bright_red(),
            crate::vm::VMState::Breakpoint => "Breakpoint".bright_yellow().bold(),
            crate::vm::VMState::Error(e) => format!("Error: {e}").bright_red().bold(),
        }
    }
    
    fn opcode_name(opcode: u8) -> &'static str {
        match opcode {
            0x00 => "HALT",
            0x01 => "ADD",
            0x02 => "SUB",
            0x03 => "AND",
            0x04 => "OR",
            0x05 => "XOR",
            0x06 => "SLL",
            0x07 => "SRL",
            0x08 => "SLT",
            0x09 => "SLTU",
            0x0A => "ADDI",
            0x0B => "ANDI",
            0x0C => "ORI",
            0x0D => "XORI",
            0x0E => "LI",
            0x0F => "SLLI",
            0x10 => "SRLI",
            0x11 => "LOAD",
            0x12 => "STORE",
            0x13 => "JAL",
            0x14 => "JALR",
            0x15 => "BEQ",
            0x16 => "BNE",
            0x17 => "BLT",
            0x18 => "BGE",
            0x19 => "BRK",
            0x1A => "MUL",
            0x1B => "DIV",
            0x1C => "MOD",
            0x1D => "MULI",
            0x1E => "DIVI",
            0x1F => "MODI",
            0x20 => "LOADC",
            0x21 => "STORC",
            0x42 => "NOP",
            _ => "UNKNOWN",
        }
    }
    
    fn register_name(reg: u8) -> String {
        let name = Register::from_u8(reg)
            .map(|r| r.to_string());

        name.unwrap_or("??".to_string())
    }
    
    fn format_operand(value: u16) -> String {
        // Check if it's a register value
        if value < 18 {
            Self::register_name(value as u8).to_string()
        } else {
            Self::format_immediate(value)
        }
    }
    
    fn format_immediate(value: u16) -> String {
        let signed = value as i16;
        if signed < 0 {
            format!("{signed}")
        } else if value > 9 {
            format!("0x{value:X}")
        } else {
            format!("{value}")
        }
    }
}