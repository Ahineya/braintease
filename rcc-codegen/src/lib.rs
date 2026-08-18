//! Ripple C99 Compiler - Code Generation Backend
//! 
//! This crate handles the final phase of compilation: generating Ripple assembly
//! from intermediate representation (IR). It includes:
//! 
//! - Assembly instruction generation
//! - Register allocation
//! - ABI implementation (calling conventions, stack frames)
//! - Ripple ISA-specific optimizations

pub mod asm;
pub mod abi;
pub mod emit;

pub use asm::{Reg, AsmInst, emit_addr_constant};
pub use abi::{Frame, AbiError};
pub use emit::{emit_instructions, CodegenError};

/// Main entry point for code generation
pub fn generate_assembly(ir: Vec<crate::asm::AsmInst>) -> Result<String, CodegenError> {
    emit_instructions(ir)
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::asm::{AsmInst, Reg};

    #[test]
    fn test_basic_code_generation() {
        let instructions = vec![
            AsmInst::Li(Reg::T0, 42),
            AsmInst::Store(Reg::T0, Reg::R0, Reg::R0),
            AsmInst::Halt,
        ];

        let result = generate_assembly(instructions);
        assert!(result.is_ok());
        
        let asm = result.unwrap();
        assert!(asm.contains("LI T0, 42"));
        assert!(asm.contains("STORE T0, R0, R0"));
        assert!(asm.contains("HALT"));
    }
}