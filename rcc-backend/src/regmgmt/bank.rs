//! Bank information management for pointers in V2 backend

use rcc_codegen::Reg;

/// Bank tag values stored in memory for fat pointers
/// 
/// When storing a fat pointer to memory, the bank component needs to be
/// represented as a value. For static banks (Global/Stack), we use special
/// tag values that are recognized when loading. For dynamic banks, we store
/// the actual bank address.
/// 
/// These are the actual runtime values stored in memory, not to be confused
/// with the compile-time BankTag enum from rcc_ir.
pub struct BankTagValue;

impl BankTagValue {
    /// Tag value for Global bank (use GP register)
    pub const GLOBAL: i16 = -1;
    
    /// Tag value for Stack bank (use SB register)
    pub const STACK: i16 = -2;
    
    /// Tag value for NULL pointers
    pub const NULL: i16 = -3;
    
    /// Check if a loaded value is a bank tag (negative value)
    pub fn is_tag(value: i16) -> bool {
        value < 0
    }
    
    /// Convert a loaded tag value to BankInfo
    /// Returns None only for NULL pointers (which shouldn't be dereferenced)
    pub fn from_value(value: i16, reg: Reg) -> Option<BankInfo> {
        match value {
            Self::GLOBAL => Some(BankInfo::Global),
            Self::STACK => Some(BankInfo::Stack),
            Self::NULL => None, // NULL pointers shouldn't be dereferenced
            _ if value >= 0 => Some(BankInfo::Register(reg)), // Positive values are dynamic bank addresses
            _ => panic!("Unknown bank tag value: {}", value), // Unknown negative values are an error
        }
    }
    
    /// Get the tag value for a BankInfo (for storing)
    pub fn to_value(bank_info: &BankInfo) -> Option<i16> {
        match bank_info {
            BankInfo::Global => Some(Self::GLOBAL),
            BankInfo::Stack => Some(Self::STACK),
            _ => None, // Dynamic banks store actual addresses, not tags
        }
    }
}

/// Information about the bank register for a pointer
#[derive(Debug, Clone)]
pub enum BankInfo {
    /// Global bank - use GP (R31)
    Global,
    
    /// Stack bank (bank 1) - use SB (R28, must be initialized!)
    Stack,
    
    /// Heap bank with specific bank number (banks 3+)
    Heap(u16),
    
    /// Dynamic bank in a register
    Register(Reg),
    
    /// Dynamic bank in a named value (can be in register or spilled)
    /// The String is the value name that can be used with get_register()
    Dynamic(String),
}

impl BankInfo {
    /// Get the register for this bank (for static banks only)
    /// For Dynamic, this will panic - use get_bank_register_with_mgr instead
    pub fn to_register(&self) -> Reg {
        match self {
            BankInfo::Global => Reg::Gp,
            BankInfo::Stack => Reg::Sb,
            BankInfo::Register(reg) => *reg,
            BankInfo::Heap(_) => {
                panic!("Cannot get register for Heap bank without allocation - heap banks require explicit register allocation")
            }
            BankInfo::Dynamic(name) => {
                panic!("Cannot get register for Dynamic('{name}') without RegisterPressureManager - use get_bank_register_with_mgr")
            }
        }
    }
    
    /// Check if this is a static bank (Global or Stack)
    pub fn is_static(&self) -> bool {
        matches!(self, BankInfo::Global | BankInfo::Stack)
    }
    
    /// Check if this requires SB initialization
    pub fn requires_sb(&self) -> bool {
        matches!(self, BankInfo::Stack)
    }
}