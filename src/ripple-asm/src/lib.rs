pub mod types;
pub mod parser;
pub mod encoder;
pub mod assembler;
pub mod linker;
mod reachability;
mod data_collection;
mod archive_selection;
pub mod macro_formatter;
mod compact_bf;
pub mod bf_cpu;
pub mod virtual_instructions;
pub mod disassembler;

#[cfg(feature = "wasm")]
pub mod wasm;

pub use types::{
    Instruction, Opcode, Register, InstructionFormat, Label, ParsedLine,
    Section, AssemblerOptions, AssemblerState, ObjectFile, UnresolvedReference,
    VirtualInstruction, Archive, ArchiveEntry,
    DEFAULT_BANK_SIZE, INSTRUCTION_SIZE, DEFAULT_MAX_IMMEDIATE,
};

pub use parser::Parser;
pub use encoder::InstructionEncoder;
pub use assembler::RippleAssembler;
pub use linker::{Linker, LinkedProgram, Reloc, RelocKind};
pub use macro_formatter::MacroFormatter;
pub use virtual_instructions::{VirtualInstructionRegistry, create_custom_instruction};
pub use disassembler::Disassembler;

// Re-export for convenience
pub fn assemble(source: &str) -> Result<ObjectFile, Vec<String>> {
    let assembler = RippleAssembler::new(AssemblerOptions::default());
    assembler.assemble(source)
}

pub fn link(object_files: Vec<ObjectFile>) -> Result<LinkedProgram, Vec<String>> {
    let linker = Linker::new(DEFAULT_BANK_SIZE);
    linker.link(object_files)
}

pub fn format_to_macro(obj: &ObjectFile) -> Result<String, Vec<String>> {
    let formatter = MacroFormatter::new();
    let image = Linker::new(DEFAULT_BANK_SIZE).link(vec![obj.clone()])?;
    Ok(formatter.format_linked_program(&image))
}
