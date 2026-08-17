//! Ripple Assembly Instruction Definitions
//! 
//! This module defines the instruction set and register model for the Ripple VM.

use std::fmt;

// Re-export Register type from ripple-asm as Reg for compatibility
pub use ripple_asm::Register as Reg;

// Helper function to display registers (if needed for custom formatting)
pub fn format_register(reg: Reg) -> String {
    reg.to_str().to_string()
}

/// Ripple Assembly Instructions
/// 
/// This enum represents all instructions that the Ripple VM can execute.
/// Instructions are categorized into arithmetic, logical, memory, control flow,
/// and pseudo-instructions for assembly generation.
#[derive(Debug, Clone, PartialEq)]
pub enum AsmInst {
    // Arithmetic Instructions
    Add(Reg, Reg, Reg),           // rd = rs + rt
    Sub(Reg, Reg, Reg),           // rd = rs - rt
    Mul(Reg, Reg, Reg),           // rd = rs * rt
    Div(Reg, Reg, Reg),           // rd = rs / rt
    Mod(Reg, Reg, Reg),           // rd = rs % rt
    
    // Arithmetic with Immediate
    AddI(Reg, Reg, i16),          // rd = rs + imm
    SubI(Reg, Reg, i16),          // rd = rs - imm
    MulI(Reg, Reg, i16),          // rd = rs * imm
    DivI(Reg, Reg, i16),          // rd = rs / imm
    ModI(Reg, Reg, i16),          // rd = rs % imm
    
    // Logical Instructions
    And(Reg, Reg, Reg),           // rd = rs & rt
    Or(Reg, Reg, Reg),            // rd = rs | rt
    Xor(Reg, Reg, Reg),           // rd = rs ^ rt
    Sll(Reg, Reg, Reg),           // rd = rs << rt
    Srl(Reg, Reg, Reg),           // rd = rs >> rt (logical)
    Slt(Reg, Reg, Reg),           // rd = (rs < rt) ? 1 : 0
    Sltu(Reg, Reg, Reg),          // rd = (rs < rt) ? 1 : 0 (unsigned)
    
    // Memory Instructions
    Load(Reg, Reg, Reg),          // rd = memory[bank:addr]
    Store(Reg, Reg, Reg),         // memory[bank:addr] = rs
    Li(Reg, i16),                 // rd = immediate
    
    // Control Flow Instructions
    Jal(i16, i16),                // Jump and link (bank_imm, addr_imm)
    Jalr(Reg, Reg, Reg),          // Jump and link register (bank_reg, addr_reg, link_reg)
    Beq(Reg, Reg, String),        // Branch if equal (rs, rt, label)
    Bne(Reg, Reg, String),        // Branch if not equal (rs, rt, label)
    Blt(Reg, Reg, String),        // Branch if less than (rs, rt, label)
    Bge(Reg, Reg, String),        // Branch if greater or equal (rs, rt, label)
    
    // Virtual/Pseudo Instructions (expand to real instructions)
    Move(Reg, Reg),               // rd = rs
    Inc(Reg),                     // rd = rd + 1
    Dec(Reg),                     // rd = rd - 1
    Push(Reg),                    // Push register to stack
    Pop(Reg),                     // Pop from stack to register
    Call(String),                 // Call function by label
    Ret,                          // Return from function
    
    // System Instructions
    Brk,                          // Breakpoint
    Halt,                         // Halt execution
    
    // Assembly Pseudo-Instructions
    Label(String),                // Label for jumps/calls
    Comment(String),              // Assembly comment
    Raw(String),                  // Raw assembly passthrough (for inline asm)
}

impl fmt::Display for AsmInst {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            // Arithmetic
            AsmInst::Add(rd, rs, rt) => write!(f, "ADD {rd}, {rs}, {rt}"),
            AsmInst::Sub(rd, rs, rt) => write!(f, "SUB {rd}, {rs}, {rt}"),
            AsmInst::Mul(rd, rs, rt) => write!(f, "MUL {rd}, {rs}, {rt}"),
            AsmInst::Div(rd, rs, rt) => write!(f, "DIV {rd}, {rs}, {rt}"),
            AsmInst::Mod(rd, rs, rt) => write!(f, "MOD {rd}, {rs}, {rt}"),
            
            // Arithmetic Immediate
            AsmInst::AddI(rd, rs, imm) => write!(f, "ADDI {rd}, {rs}, {imm}"),
            AsmInst::SubI(rd, rs, imm) => write!(f, "SUBI {rd}, {rs}, {imm}"),
            AsmInst::MulI(rd, rs, imm) => write!(f, "MULI {rd}, {rs}, {imm}"),
            AsmInst::DivI(rd, rs, imm) => write!(f, "DIVI {rd}, {rs}, {imm}"),
            AsmInst::ModI(rd, rs, imm) => write!(f, "MODI {rd}, {rs}, {imm}"),
            
            // Logical
            AsmInst::And(rd, rs, rt) => write!(f, "AND {rd}, {rs}, {rt}"),
            AsmInst::Or(rd, rs, rt) => write!(f, "OR {rd}, {rs}, {rt}"),
            AsmInst::Xor(rd, rs, rt) => write!(f, "XOR {rd}, {rs}, {rt}"),
            AsmInst::Sll(rd, rs, rt) => write!(f, "SLL {rd}, {rs}, {rt}"),
            AsmInst::Srl(rd, rs, rt) => write!(f, "SRL {rd}, {rs}, {rt}"),
            AsmInst::Slt(rd, rs, rt) => write!(f, "SLT {rd}, {rs}, {rt}"),
            AsmInst::Sltu(rd, rs, rt) => write!(f, "SLTU {rd}, {rs}, {rt}"),
            
            // Memory
            AsmInst::Load(rd, bank, addr) => write!(f, "LOAD {rd}, {bank}, {addr}"),
            AsmInst::Store(rs, bank, addr) => write!(f, "STORE {rs}, {bank}, {addr}"),
            AsmInst::Li(rd, imm) => write!(f, "LI {rd}, {imm}"),
            
            // Control Flow
            AsmInst::Jal(bank, addr) => write!(f, "JAL RA, {bank}, {addr}"),
            AsmInst::Jalr(bank, addr, link) => write!(f, "JALR {bank}, {addr}, {link}"),
            AsmInst::Beq(rs, rt, label) => write!(f, "BEQ {rs}, {rt}, {label}"),
            AsmInst::Bne(rs, rt, label) => write!(f, "BNE {rs}, {rt}, {label}"),
            AsmInst::Blt(rs, rt, label) => write!(f, "BLT {rs}, {rt}, {label}"),
            AsmInst::Bge(rs, rt, label) => write!(f, "BGE {rs}, {rt}, {label}"),
            
            // Virtual/Pseudo
            AsmInst::Move(rd, rs) => write!(f, "MOVE {rd}, {rs}"),
            AsmInst::Inc(rd) => write!(f, "INC {rd}"),
            AsmInst::Dec(rd) => write!(f, "DEC {rd}"),
            AsmInst::Push(rs) => write!(f, "PUSH {rs}"),
            AsmInst::Pop(rd) => write!(f, "POP {rd}"),
            AsmInst::Call(label) => write!(f, "CALL {label}"),
            AsmInst::Ret => write!(f, "RET"),
            
            // System
            AsmInst::Brk => write!(f, "BRK"),
            AsmInst::Halt => write!(f, "HALT"),
            
            // Pseudo
            AsmInst::Label(label) => write!(f, "{label}:"),
            AsmInst::Comment(text) => write!(f, "; {text}"),
            AsmInst::Raw(asm) => write!(f, "{asm}"),
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_register_display() {
        assert_eq!(Reg::R0.to_str(), "R0");
        assert_eq!(Reg::T0.to_str(), "T0");
        assert_eq!(Reg::Pc.to_str(), "PC");
        assert_eq!(Reg::Ra.to_str(), "RA");
    }

    #[test]
    fn test_instruction_display() {
        assert_eq!(format!("{}", AsmInst::Li(Reg::T0, 42)), "LI T0, 42");
        assert_eq!(format!("{}", AsmInst::Add(Reg::T0, Reg::T1, Reg::T2)), "ADD T0, T1, T2");
        assert_eq!(format!("{}", AsmInst::Store(Reg::T0, Reg::R0, Reg::R0)), "STORE T0, R0, R0");
        assert_eq!(format!("{}", AsmInst::Jal(1, 42)), "JAL RA, 1, 42");
        assert_eq!(format!("{}", AsmInst::Label("main".to_string())), "main:");
        assert_eq!(format!("{}", AsmInst::Comment("Hello world".to_string())), "; Hello world");
    }

    #[test]
    fn test_hello_world_instructions() {
        let instructions = vec![
            AsmInst::Label("main".to_string()),
            AsmInst::Li(Reg::T0, 'H' as i16),
            AsmInst::Store(Reg::T0, Reg::R0, Reg::R0),
            AsmInst::Li(Reg::T0, 'i' as i16),
            AsmInst::Store(Reg::T0, Reg::R0, Reg::R0),
            AsmInst::Li(Reg::T0, '\n' as i16),
            AsmInst::Store(Reg::T0, Reg::R0, Reg::R0),
            AsmInst::Halt,
        ];

        for inst in instructions {
            println!("{}", inst);
        }
    }
}