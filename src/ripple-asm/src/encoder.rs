use crate::types::{Instruction, Opcode, Register, InstructionFormat};

pub struct InstructionEncoder {
    max_immediate: u32,
}

impl InstructionEncoder {
    pub fn new(max_immediate: u32) -> Self {
        Self { max_immediate }
    }

    pub fn encode(
        &self,
        opcode: Opcode,
        operands: &[String],
    ) -> Result<Instruction, String> {
        match opcode.format() {
            InstructionFormat::R => self.encode_r_format(opcode, operands),
            InstructionFormat::I => self.encode_i_format(opcode, operands),
            InstructionFormat::I1 => self.encode_i1_format(opcode, operands),
        }
    }

    fn encode_r_format(
        &self,
        opcode: Opcode,
        operands: &[String],
    ) -> Result<Instruction, String> {
        // Special case for HALT (NOP with no operands)
        if opcode == Opcode::Nop && operands.is_empty() {
            return Ok(Instruction::new(opcode, 0, 0, 0));
        }

        // Special case for BRK
        if opcode == Opcode::Brk && operands.is_empty() {
            return Ok(Instruction::new(opcode, 0, 0, 0));
        }

        if operands.len() != 3 {
            return Err(format!(
                "{} requires 3 operands, got {}",
                opcode.to_str(),
                operands.len()
            ));
        }

        let rd = self.parse_register(&operands[0])?;
        let rs1 = self.parse_register(&operands[1])?;
        let rs2 = self.parse_register(&operands[2])?;

        Ok(Instruction::new(opcode, rd as u16, rs1 as u16, rs2 as u16))
    }

    fn encode_i_format(
        &self,
        opcode: Opcode,
        operands: &[String],
    ) -> Result<Instruction, String> {
        if operands.len() != 3 {
            return Err(format!(
                "{} requires 3 operands, got {}",
                opcode.to_str(),
                operands.len()
            ));
        }

        match opcode {
            Opcode::Load => {
                // LOAD rd, bank, addr - all can be registers
                let rd = self.parse_register(&operands[0])?;
                let bank = self.parse_register_or_immediate(&operands[1])?;
                let addr = self.parse_register_or_immediate(&operands[2])?;
                Ok(Instruction::new(opcode, rd as u16, bank, addr))
            }
            Opcode::Store => {
                // STORE rs, bank, addr
                let rs = self.parse_register(&operands[0])?;
                let bank = self.parse_register_or_immediate(&operands[1])?;
                let addr = self.parse_register_or_immediate(&operands[2])?;
                Ok(Instruction::new(opcode, rs as u16, bank, addr))
            }
            Opcode::Jal => {
                // JAL rd, offset_high, offset_low (or label)
                let rd = self.parse_register_or_immediate(&operands[0])?;
                let offset_high = self.parse_register_or_immediate(&operands[1])?;
                let offset_low = self.parse_register_or_immediate(&operands[2])?;
                Ok(Instruction::new(opcode, rd, offset_high, offset_low))
            }
            Opcode::Beq | Opcode::Bne | Opcode::Blt | Opcode::Bge => {
                // Branch: rs1, rs2, offset
                let rs1 = self.parse_register(&operands[0])?;
                let rs2 = self.parse_register(&operands[1])?;
                let offset = self.parse_immediate(&operands[2])?;
                Ok(Instruction::new(opcode, rs1 as u16, rs2 as u16, offset))
            }
            _ => {
                // Generic I-format: rd, rs, imm
                let rd = self.parse_register(&operands[0])?;
                let rs = self.parse_register(&operands[1])?;
                let imm = self.parse_immediate(&operands[2])?;
                Ok(Instruction::new(opcode, rd as u16, rs as u16, imm))
            }
        }
    }

    fn encode_i1_format(
        &self,
        opcode: Opcode,
        operands: &[String],
    ) -> Result<Instruction, String> {
        // LI rd, immediate
        if operands.len() == 2 {
            let rd = self.parse_register(&operands[0])?;
            let imm = self.parse_immediate(&operands[1])?;
            Ok(Instruction::new(opcode, rd as u16, imm, 0))
        } else if operands.len() == 3 {
            // LI rd, imm_high, imm_low (for large immediates)
            let rd = self.parse_register(&operands[0])?;
            let imm_high = self.parse_immediate(&operands[1])?;
            let imm_low = self.parse_immediate(&operands[2])?;
            Ok(Instruction::new(opcode, rd as u16, imm_high, imm_low))
        } else {
            Err(format!(
                "{} requires 2 or 3 operands, got {}",
                opcode.to_str(),
                operands.len()
            ))
        }
    }

    pub fn parse_register(&self, operand: &str) -> Result<u8, String> {
        Register::from_str(operand)
            .map(|r| r as u8)
            .ok_or_else(|| format!("Invalid register: {}", operand))
    }

    fn parse_register_or_immediate(&self, operand: &str) -> Result<u16, String> {
        // Try to parse as register first
        if let Some(reg) = Register::from_str(operand) {
            return Ok(reg as u16);
        }
        
        // Otherwise parse as immediate
        self.parse_immediate(operand)
    }

    pub fn parse_immediate(&self, operand: &str) -> Result<u16, String> {
        // Handle negative numbers (parse as i32 then cast)
        if operand.starts_with("-") {
            let value = operand.parse::<i32>()
                .map_err(|_| format!("Invalid number: {}", operand))?;
            // Cast to u16 (this will wrap for negative values, which is what we want)
            return Ok(value as u16);
        }
        
        // Handle hex numbers
        let value = if operand.starts_with("0x") || operand.starts_with("0X") {
            u32::from_str_radix(&operand[2..], 16)
                .map_err(|_| format!("Invalid hex number: {}", operand))?
        }
        // Handle binary numbers
        else if operand.starts_with("0b") || operand.starts_with("0B") {
            u32::from_str_radix(&operand[2..], 2)
                .map_err(|_| format!("Invalid binary number: {}", operand))?
        }
        // Handle decimal numbers
        else {
            operand.parse::<u32>()
                .map_err(|_| format!("Invalid number: {}", operand))?
        };

        if value > self.max_immediate {
            return Err(format!(
                "Immediate value {} exceeds maximum {}",
                value, self.max_immediate
            ));
        }

        Ok(value as u16)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_encode_add() {
        let encoder = InstructionEncoder::new(65535);
        let inst = encoder.encode(Opcode::Add, &["R3".to_string(), "R4".to_string(), "R5".to_string()]).unwrap();
        
        assert_eq!(inst.opcode, Opcode::Add as u8);
        assert_eq!(inst.word1, Register::Ra as u16);
        assert_eq!(inst.word2, Register::Rab as u16);
        assert_eq!(inst.word3, Register::Rv0 as u16);
    }

    #[test]
    fn test_encode_li() {
        let encoder = InstructionEncoder::new(65535);
        let inst = encoder.encode(Opcode::Li, &["R3".to_string(), "42".to_string()]).unwrap();
        
        assert_eq!(inst.opcode, Opcode::Li as u8);
        assert_eq!(inst.word1, Register::Ra as u16);
        assert_eq!(inst.word2, 42);
        assert_eq!(inst.word3, 0);
    }

    #[test]
    fn test_encode_halt() {
        let encoder = InstructionEncoder::new(65535);
        let inst = encoder.encode(Opcode::Nop, &[]).unwrap();
        
        assert_eq!(inst.opcode, Opcode::Nop as u8);
        assert_eq!(inst.word1, 0);
        assert_eq!(inst.word2, 0);
        assert_eq!(inst.word3, 0);
        assert!(inst.is_halt());
    }

    #[test]
    fn test_parse_hex_immediate() {
        let encoder = InstructionEncoder::new(65535);
        assert_eq!(encoder.parse_immediate("0xFF").unwrap(), 255);
        assert_eq!(encoder.parse_immediate("0x1234").unwrap(), 0x1234);
    }

    #[test]
    fn test_parse_binary_immediate() {
        let encoder = InstructionEncoder::new(65535);
        assert_eq!(encoder.parse_immediate("0b1010").unwrap(), 10);
        assert_eq!(encoder.parse_immediate("0b11111111").unwrap(), 255);
    }
}