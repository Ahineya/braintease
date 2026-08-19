/// Instruction representation for the Ripple VM
#[derive(Debug, Clone, Copy)]
pub struct Instr {
    pub opcode: u8,
    pub word0: u8,
    pub word1: u16,
    pub word2: u16,
    pub word3: u16,
}

impl Instr {
    #[allow(dead_code)]
    pub fn new(opcode: u8, word0: u8, word1: u16, word2: u16, word3: u16) -> Self {
        Self { opcode, word0, word1, word2, word3 }
    }
    
    #[allow(dead_code)]
    pub fn from_bytes(bytes: &[u8]) -> Option<Self> {
        if bytes.len() < 8 {
            return None;
        }
        Some(Self {
            opcode: bytes[0],
            word0: bytes[1],
            word1: u16::from_le_bytes([bytes[2], bytes[3]]),
            word2: u16::from_le_bytes([bytes[4], bytes[5]]),
            word3: u16::from_le_bytes([bytes[6], bytes[7]]),
        })
    }
    
    #[allow(dead_code)]
    pub fn is_halt(&self) -> bool {
        self.opcode == 0x00
    }
}