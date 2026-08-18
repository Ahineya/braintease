use serde::{Deserialize, Serialize};
use std::collections::HashMap;
use std::hash::Hash;

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[repr(u8)]
pub enum Opcode {
    Nop = 0x00,
    Add = 0x01,
    Sub = 0x02,
    And = 0x03,
    Or = 0x04,
    Xor = 0x05,
    Sll = 0x06,
    Srl = 0x07,
    Slt = 0x08,
    Sltu = 0x09,
    Addi = 0x0A,
    Andi = 0x0B,
    Ori = 0x0C,
    Xori = 0x0D,
    Li = 0x0E,
    Slli = 0x0F,
    Srli = 0x10,
    Load = 0x11,
    Store = 0x12,
    Jal = 0x13,
    Jalr = 0x14,
    Beq = 0x15,
    Bne = 0x16,
    Blt = 0x17,
    Bge = 0x18,
    Brk = 0x19,
    Mul = 0x1A,
    Div = 0x1B,
    Mod = 0x1C,
    Muli = 0x1D,
    Divi = 0x1E,
    Modi = 0x1F,
    Loadc = 0x20,
    Storc = 0x21,
}

impl Opcode {
    pub fn from_str(s: &str) -> Option<Self> {
        match s.to_uppercase().as_str() {
            "NOP" => Some(Opcode::Nop),
            "ADD" => Some(Opcode::Add),
            "SUB" => Some(Opcode::Sub),
            "AND" => Some(Opcode::And),
            "OR" => Some(Opcode::Or),
            "XOR" => Some(Opcode::Xor),
            "SLL" => Some(Opcode::Sll),
            "SRL" => Some(Opcode::Srl),
            "SLT" => Some(Opcode::Slt),
            "SLTU" => Some(Opcode::Sltu),
            "ADDI" => Some(Opcode::Addi),
            "ANDI" => Some(Opcode::Andi),
            "ORI" => Some(Opcode::Ori),
            "XORI" => Some(Opcode::Xori),
            "LI" => Some(Opcode::Li),
            "SLLI" => Some(Opcode::Slli),
            "SRLI" => Some(Opcode::Srli),
            "LOAD" => Some(Opcode::Load),
            "STORE" => Some(Opcode::Store),
            "JAL" => Some(Opcode::Jal),
            "JALR" => Some(Opcode::Jalr),
            "BEQ" => Some(Opcode::Beq),
            "BNE" => Some(Opcode::Bne),
            "BLT" => Some(Opcode::Blt),
            "BGE" => Some(Opcode::Bge),
            "BRK" => Some(Opcode::Brk),
            "MUL" => Some(Opcode::Mul),
            "DIV" => Some(Opcode::Div),
            "MOD" => Some(Opcode::Mod),
            "MULI" => Some(Opcode::Muli),
            "DIVI" => Some(Opcode::Divi),
            "MODI" => Some(Opcode::Modi),
            "LOADC" => Some(Opcode::Loadc),
            "STORC" => Some(Opcode::Storc),
            _ => None,
        }
    }

    pub fn from_u8(value: u8) -> Option<Self> {
        match value {
            0x00 => Some(Opcode::Nop),
            0x01 => Some(Opcode::Add),
            0x02 => Some(Opcode::Sub),
            0x03 => Some(Opcode::And),
            0x04 => Some(Opcode::Or),
            0x05 => Some(Opcode::Xor),
            0x06 => Some(Opcode::Sll),
            0x07 => Some(Opcode::Srl),
            0x08 => Some(Opcode::Slt),
            0x09 => Some(Opcode::Sltu),
            0x0A => Some(Opcode::Addi),
            0x0B => Some(Opcode::Andi),
            0x0C => Some(Opcode::Ori),
            0x0D => Some(Opcode::Xori),
            0x0E => Some(Opcode::Li),
            0x0F => Some(Opcode::Slli),
            0x10 => Some(Opcode::Srli),
            0x11 => Some(Opcode::Load),
            0x12 => Some(Opcode::Store),
            0x13 => Some(Opcode::Jal),
            0x14 => Some(Opcode::Jalr),
            0x15 => Some(Opcode::Beq),
            0x16 => Some(Opcode::Bne),
            0x17 => Some(Opcode::Blt),
            0x18 => Some(Opcode::Bge),
            0x19 => Some(Opcode::Brk),
            0x1A => Some(Opcode::Mul),
            0x1B => Some(Opcode::Div),
            0x1C => Some(Opcode::Mod),
            0x1D => Some(Opcode::Muli),
            0x1E => Some(Opcode::Divi),
            0x1F => Some(Opcode::Modi),
            0x20 => Some(Opcode::Loadc),
            0x21 => Some(Opcode::Storc),
            _ => None,
        }
    }

    pub fn to_str(&self) -> &'static str {
        match self {
            Opcode::Nop => "NOP",
            Opcode::Add => "ADD",
            Opcode::Sub => "SUB",
            Opcode::And => "AND",
            Opcode::Or => "OR",
            Opcode::Xor => "XOR",
            Opcode::Sll => "SLL",
            Opcode::Srl => "SRL",
            Opcode::Slt => "SLT",
            Opcode::Sltu => "SLTU",
            Opcode::Addi => "ADDI",
            Opcode::Andi => "ANDI",
            Opcode::Ori => "ORI",
            Opcode::Xori => "XORI",
            Opcode::Li => "LI",
            Opcode::Slli => "SLLI",
            Opcode::Srli => "SRLI",
            Opcode::Load => "LOAD",
            Opcode::Store => "STORE",
            Opcode::Jal => "JAL",
            Opcode::Jalr => "JALR",
            Opcode::Beq => "BEQ",
            Opcode::Bne => "BNE",
            Opcode::Blt => "BLT",
            Opcode::Bge => "BGE",
            Opcode::Brk => "BRK",
            Opcode::Mul => "MUL",
            Opcode::Div => "DIV",
            Opcode::Mod => "MOD",
            Opcode::Muli => "MULI",
            Opcode::Divi => "DIVI",
            Opcode::Modi => "MODI",
            Opcode::Loadc => "LOADC",
            Opcode::Storc => "STORC",
        }
    }

    pub fn all() -> Vec<&'static str> {
        vec![
            "NOP", "ADD", "SUB", "AND", "OR", "XOR", "SLL", "SRL", "SLT", "SLTU",
            "ADDI", "ANDI", "ORI", "XORI", "LI", "SLLI", "SRLI", "LOAD", "STORE",
            "JAL", "JALR", "BEQ", "BNE", "BLT", "BGE", "BRK", "MUL", "DIV", "MOD",
            "MULI", "DIVI", "MODI", "LOADC", "STORC"
        ]
    }

    pub fn format(&self) -> InstructionFormat {
        match self {
            Opcode::Nop | Opcode::Add | Opcode::Sub | Opcode::And | Opcode::Or | 
            Opcode::Xor | Opcode::Sll | Opcode::Srl | Opcode::Slt | Opcode::Sltu | 
            Opcode::Jalr | Opcode::Brk | Opcode::Mul | Opcode::Div | Opcode::Mod |
            Opcode::Loadc | Opcode::Storc => InstructionFormat::R,
            
            Opcode::Addi | Opcode::Andi | Opcode::Ori | Opcode::Xori | 
            Opcode::Slli | Opcode::Srli | Opcode::Load | Opcode::Store | 
            Opcode::Beq | Opcode::Bne | Opcode::Blt | Opcode::Bge | 
            Opcode::Jal | Opcode::Muli | Opcode::Divi | Opcode::Modi => InstructionFormat::I,
            
            Opcode::Li => InstructionFormat::I1,
        }
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub enum Register {
    R0 = 0,   // Zero register (ZR)
    Pc = 1,   // Program Counter
    Pcb = 2,  // Program Counter Bank
    Ra = 3,   // Return Address
    Rab = 4,  // Return Address Bank
    Rv0 = 5,  // Return Value 0 (R5)
    Rv1 = 6,  // Return Value 1 (R6)
    A0 = 7,   // Argument 0 (R7)
    A1 = 8,   // Argument 1 (R8)
    A2 = 9,   // Argument 2 (R9)
    A3 = 10,  // Argument 3 (R10)
    X0 = 11,  // Reserved/Extended 0 (R11)
    X1 = 12,  // Reserved/Extended 1 (R12)
    X2 = 13,  // Reserved/Extended 2 (R13)
    X3 = 14,  // Reserved/Extended 3 (R14)
    T0 = 15,  // Temporary 0 (R15)
    T1 = 16,  // Temporary 1 (R16)
    T2 = 17,  // Temporary 2 (R17)
    T3 = 18,  // Temporary 3 (R18)
    T4 = 19,  // Temporary 4 (R19)
    T5 = 20,  // Temporary 5 (R20)
    T6 = 21,  // Temporary 6 (R21)
    T7 = 22,  // Temporary 7 (R22)
    S0 = 23,  // Saved 0 (R23)
    S1 = 24,  // Saved 1 (R24)
    S2 = 25,  // Saved 2 (R25)
    S3 = 26,  // Saved 3 (R26)
    Sc = 27,  // Allocator Scratch (R27)
    Sb = 28,  // Stack Bank (R28)
    Sp = 29,  // Stack Pointer (R29)
    Fp = 30,  // Frame Pointer (R30)
    Gp = 31,  // Global Pointer (R31)
}

impl std::fmt::Display for Register {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        write!(f, "{}", self.to_str())
    }
}

impl Register {
    pub fn from_u8(value: u8) -> Option<Self> {
        match value {
            0 => Some(Register::R0),
            1 => Some(Register::Pc),
            2 => Some(Register::Pcb),
            3 => Some(Register::Ra),
            4 => Some(Register::Rab),
            5 => Some(Register::Rv0),
            6 => Some(Register::Rv1),
            7 => Some(Register::A0),
            8 => Some(Register::A1),
            9 => Some(Register::A2),
            10 => Some(Register::A3),
            11 => Some(Register::X0),
            12 => Some(Register::X1),
            13 => Some(Register::X2),
            14 => Some(Register::X3),
            15 => Some(Register::T0),
            16 => Some(Register::T1),
            17 => Some(Register::T2),
            18 => Some(Register::T3),
            19 => Some(Register::T4),
            20 => Some(Register::T5),
            21 => Some(Register::T6),
            22 => Some(Register::T7),
            23 => Some(Register::S0),
            24 => Some(Register::S1),
            25 => Some(Register::S2),
            26 => Some(Register::S3),
            27 => Some(Register::Sc),
            28 => Some(Register::Sb),
            29 => Some(Register::Sp),
            30 => Some(Register::Fp),
            31 => Some(Register::Gp),
            _ => None,
        }
    }

    pub fn from_str(s: &str) -> Option<Self> {
        let upper = s.to_uppercase();
        
        // Try symbolic names first
        match upper.as_str() {
            "ZR" | "R0" => Some(Register::R0),
            "PC" | "R1" => Some(Register::Pc),
            "PCB" | "R2" => Some(Register::Pcb),
            "RA" | "R3" => Some(Register::Ra),
            "RAB" | "R4" => Some(Register::Rab),
            "RV0" | "V0" | "R5" => Some(Register::Rv0),
            "RV1" | "V1" | "R6" => Some(Register::Rv1),
            "A0" | "R7" => Some(Register::A0),
            "A1" | "R8" => Some(Register::A1),
            "A2" | "R9" => Some(Register::A2),
            "A3" | "R10" => Some(Register::A3),
            "X0" | "R11" => Some(Register::X0),
            "X1" | "R12" => Some(Register::X1),
            "X2" | "R13" => Some(Register::X2),
            "X3" | "R14" => Some(Register::X3),
            "T0" | "R15" => Some(Register::T0),
            "T1" | "R16" => Some(Register::T1),
            "T2" | "R17" => Some(Register::T2),
            "T3" | "R18" => Some(Register::T3),
            "T4" | "R19" => Some(Register::T4),
            "T5" | "R20" => Some(Register::T5),
            "T6" | "R21" => Some(Register::T6),
            "T7" | "R22" => Some(Register::T7),
            "S0" | "R23" => Some(Register::S0),
            "S1" | "R24" => Some(Register::S1),
            "S2" | "R25" => Some(Register::S2),
            "S3" | "R26" => Some(Register::S3),
            "SC" | "R27" => Some(Register::Sc),
            "SB" | "R28" => Some(Register::Sb),
            "SP" | "R29" => Some(Register::Sp),
            "FP" | "R30" => Some(Register::Fp),
            "GP" | "R31" => Some(Register::Gp),
            _ => None,
        }
    }
    
    pub fn to_str(&self) -> &'static str {
        match self {
            Register::R0 => "R0",
            Register::Pc => "PC",
            Register::Pcb => "PCB",
            Register::Ra => "RA",
            Register::Rab => "RAB",
            Register::Rv0 => "RV0",
            Register::Rv1 => "RV1",
            Register::A0 => "A0",
            Register::A1 => "A1",
            Register::A2 => "A2",
            Register::A3 => "A3",
            Register::X0 => "X0",
            Register::X1 => "X1",
            Register::X2 => "X2",
            Register::X3 => "X3",
            Register::T0 => "T0",
            Register::T1 => "T1",
            Register::T2 => "T2",
            Register::T3 => "T3",
            Register::T4 => "T4",
            Register::T5 => "T5",
            Register::T6 => "T6",
            Register::T7 => "T7",
            Register::S0 => "S0",
            Register::S1 => "S1",
            Register::S2 => "S2",
            Register::S3 => "S3",
            Register::Sc => "SC",
            Register::Sb => "SB",
            Register::Sp => "SP",
            Register::Fp => "FP",
            Register::Gp => "GP",
        }
    }

    pub fn to_macro_str(&self) -> &'static str {
        match self {
            Register::R0 => "@R0",
            Register::Pc => "@PC",
            Register::Pcb => "@PCB",
            Register::Ra => "@RA",
            Register::Rab => "@RAB",
            Register::Rv0 => "@RV0",
            Register::Rv1 => "@RV1",
            Register::A0 => "@A0",
            Register::A1 => "@A1",
            Register::A2 => "@A2",
            Register::A3 => "@A3",
            Register::X0 => "@X0",
            Register::X1 => "@X1",
            Register::X2 => "@X2",
            Register::X3 => "@X3",
            Register::T0 => "@T0",
            Register::T1 => "@T1",
            Register::T2 => "@T2",
            Register::T3 => "@T3",
            Register::T4 => "@T4",
            Register::T5 => "@T5",
            Register::T6 => "@T6",
            Register::T7 => "@T7",
            Register::S0 => "@S0",
            Register::S1 => "@S1",
            Register::S2 => "@S2",
            Register::S3 => "@S3",
            Register::Sc => "@SC",
            Register::Sb => "@SB",
            Register::Sp => "@SP",
            Register::Fp => "@FP",
            Register::Gp => "@GP",
        }
    }

    pub fn all() -> Vec<&'static str> {
        vec![
            "R0", "PC", "PCB", "RA", "RAB", "RV0", "RV1", 
            "A0", "A1", "A2", "A3", "X0", "X1", "X2", "X3",
            "T0", "T1", "T2", "T3", "T4", "T5", "T6", "T7",
            "S0", "S1", "S2", "S3", "SC", "SB", "SP", "FP", "GP"
        ]
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum InstructionFormat {
    R,  // Register format
    I,  // Immediate format
    I1, // Special immediate format for LI
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
pub struct Instruction {
    pub opcode: u8,
    pub word0: u8,
    pub word1: u16,
    pub word2: u16,
    pub word3: u16,
}

impl Instruction {
    pub fn new(opcode: Opcode, word1: u16, word2: u16, word3: u16) -> Self {
        Self {
            opcode: opcode as u8,
            word0: opcode as u8,
            word1,
            word2,
            word3,
        }
    }

    pub fn is_halt(&self) -> bool {
        self.opcode == Opcode::Nop as u8 && 
        self.word1 == 0 && 
        self.word2 == 0 && 
        self.word3 == 0
    }
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct Label {
    pub name: String,
    pub bank: u16,
    pub offset: u16,
    pub absolute_address: u32,
}

#[derive(Debug, Clone)]
pub struct ParsedLine {
    pub label: Option<String>,
    pub mnemonic: Option<String>,
    pub operands: Vec<String>,
    pub directive: Option<String>,
    pub directive_args: Vec<String>,
    pub line_number: usize,
    pub raw: String,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Section {
    Code,
    Data,
}

#[derive(Debug)]
pub struct AssemblerOptions {
    pub case_insensitive: bool,
    pub start_bank: u16,
    pub bank_size: u16,
    pub max_immediate: u32,
    pub memory_offset: u16,  // Offset to add to all memory addresses (default 2 for VM special values)
}

impl Default for AssemblerOptions {
    fn default() -> Self {
        Self {
            case_insensitive: true,
            start_bank: 0,
            bank_size: DEFAULT_BANK_SIZE,
            max_immediate: DEFAULT_MAX_IMMEDIATE,
            memory_offset: 2,  // Default to 2 to account for VM special values
        }
    }
}

pub const DEFAULT_BANK_SIZE: u16 = 16;
pub const INSTRUCTION_SIZE: u16 = 4;
pub const DEFAULT_MAX_IMMEDIATE: u32 = 65535;

#[derive(Debug)]
pub struct AssemblerState {
    pub current_bank: u16,
    pub current_offset: u16,
    pub labels: HashMap<String, Label>,
    pub data_labels: HashMap<String, u32>,
    pub pending_references: HashMap<usize, PendingReference>,
    pub instructions: Vec<Instruction>,
    pub memory_data: Vec<u8>,
    pub errors: Vec<String>,
}

#[derive(Debug, Clone)]
pub struct PendingReference {
    pub label: String,
    pub ref_type: ReferenceType,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum ReferenceType {
    Branch,
    Absolute,
    Data,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct ObjectFile {
    pub version: u32,
    pub instructions: Vec<Instruction>,
    pub data: Vec<u8>,
    pub labels: HashMap<String, Label>,
    pub data_labels: HashMap<String, u32>,
    pub unresolved_references: HashMap<usize, UnresolvedReference>,
    pub entry_point: Option<String>,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct UnresolvedReference {
    pub label: String,
    pub ref_type: String, // "branch", "absolute", "data"
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct Archive {
    pub version: u32,
    pub objects: Vec<ArchiveEntry>,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct ArchiveEntry {
    pub name: String,  // Original filename or module name
    pub object: ObjectFile,
}

// Virtual instruction definitions for extensibility
pub trait VirtualInstruction {
    fn name(&self) -> &str;
    fn expand(&self, operands: &[String]) -> Result<Vec<ParsedLine>, String>;
}