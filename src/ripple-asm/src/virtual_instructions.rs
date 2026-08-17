use crate::types::{ParsedLine, VirtualInstruction};
use std::collections::HashMap;

pub struct VirtualInstructionRegistry {
    instructions: HashMap<String, Box<dyn VirtualInstruction>>,
}

impl VirtualInstructionRegistry {
    pub fn new() -> Self {
        let mut registry = Self {
            instructions: HashMap::new(),
        };
        
        // Register default virtual instructions
        registry.register(Box::new(MoveInstruction));
        registry.register(Box::new(PushInstruction));
        registry.register(Box::new(PopInstruction));
        registry.register(Box::new(CallInstruction));
        registry.register(Box::new(RetInstruction));
        registry.register(Box::new(IncInstruction));
        registry.register(Box::new(DecInstruction));
        registry.register(Box::new(NegInstruction));
        registry.register(Box::new(NotInstruction));
        registry.register(Box::new(SubiInstruction));
        
        registry
    }

    pub fn register(&mut self, instruction: Box<dyn VirtualInstruction>) {
        self.instructions.insert(
            instruction.name().to_uppercase(),
            instruction,
        );
    }

    pub fn get(&self, name: &str) -> Option<&Box<dyn VirtualInstruction>> {
        self.instructions.get(&name.to_uppercase())
    }

    pub fn get_all_names(&self) -> Vec<String> {
        self.instructions.keys().cloned().collect()
    }
}

// MOVE rd, rs - Copy value from rs to rd
struct MoveInstruction;
impl VirtualInstruction for MoveInstruction {
    fn name(&self) -> &str { "MOVE" }
    
    fn expand(&self, operands: &[String]) -> Result<Vec<ParsedLine>, String> {
        if operands.len() != 2 {
            return Err(format!("MOVE requires 2 operands, got {}", operands.len()));
        }
        
        Ok(vec![ParsedLine {
            label: None,
            mnemonic: Some("ADD".to_string()),
            operands: vec![operands[0].clone(), operands[1].clone(), "R0".to_string()],
            directive: None,
            directive_args: Vec::new(),
            line_number: 0,
            raw: String::new(),
        }])
    }
}

// PUSH rs - Push register to stack (using R15 as stack pointer)
struct PushInstruction;
impl VirtualInstruction for PushInstruction {
    fn name(&self) -> &str { "PUSH" }
    
    fn expand(&self, operands: &[String]) -> Result<Vec<ParsedLine>, String> {
        if operands.len() != 1 {
            return Err(format!("PUSH requires 1 operand, got {}", operands.len()));
        }
        
        Ok(vec![
            ParsedLine {
                label: None,
                mnemonic: Some("ADDI".to_string()),
                operands: vec!["R15".to_string(), "R15".to_string(), "-1".to_string()],
                directive: None,
                directive_args: Vec::new(),
                line_number: 0,
                raw: String::new(),
            },
            ParsedLine {
                label: None,
                mnemonic: Some("STORE".to_string()),
                operands: vec![operands[0].clone(), "R0".to_string(), "R15".to_string()],
                directive: None,
                directive_args: Vec::new(),
                line_number: 0,
                raw: String::new(),
            },
        ])
    }
}

// POP rd - Pop from stack to register
struct PopInstruction;
impl VirtualInstruction for PopInstruction {
    fn name(&self) -> &str { "POP" }
    
    fn expand(&self, operands: &[String]) -> Result<Vec<ParsedLine>, String> {
        if operands.len() != 1 {
            return Err(format!("POP requires 1 operand, got {}", operands.len()));
        }
        
        Ok(vec![
            ParsedLine {
                label: None,
                mnemonic: Some("LOAD".to_string()),
                operands: vec![operands[0].clone(), "R0".to_string(), "R15".to_string()],
                directive: None,
                directive_args: Vec::new(),
                line_number: 0,
                raw: String::new(),
            },
            ParsedLine {
                label: None,
                mnemonic: Some("ADDI".to_string()),
                operands: vec!["R15".to_string(), "R15".to_string(), "1".to_string()],
                directive: None,
                directive_args: Vec::new(),
                line_number: 0,
                raw: String::new(),
            },
        ])
    }
}

// CALL label - Call subroutine (save return address)
struct CallInstruction;
impl VirtualInstruction for CallInstruction {
    fn name(&self) -> &str { "CALL" }
    
    fn expand(&self, operands: &[String]) -> Result<Vec<ParsedLine>, String> {
        if operands.len() != 1 {
            return Err(format!("CALL requires 1 operand, got {}", operands.len()));
        }
        
        Ok(vec![ParsedLine {
            label: None,
            mnemonic: Some("JAL".to_string()),
            operands: vec!["RA".to_string(), "R0".to_string(), operands[0].clone()],
            directive: None,
            directive_args: Vec::new(),
            line_number: 0,
            raw: String::new(),
        }])
    }
}

// RET - Return from subroutine
struct RetInstruction;
impl VirtualInstruction for RetInstruction {
    fn name(&self) -> &str { "RET" }
    
    fn expand(&self, operands: &[String]) -> Result<Vec<ParsedLine>, String> {
        if !operands.is_empty() {
            return Err(format!("RET takes no operands, got {}", operands.len()));
        }
        
        Ok(vec![ParsedLine {
            label: None,
            mnemonic: Some("JALR".to_string()),
            operands: vec!["R0".to_string(), "RAB".to_string(), "RA".to_string()],
            directive: None,
            directive_args: Vec::new(),
            line_number: 0,
            raw: String::new(),
        }])
    }
}

// INC rd - Increment register
struct IncInstruction;
impl VirtualInstruction for IncInstruction {
    fn name(&self) -> &str { "INC" }
    
    fn expand(&self, operands: &[String]) -> Result<Vec<ParsedLine>, String> {
        if operands.len() != 1 {
            return Err(format!("INC requires 1 operand, got {}", operands.len()));
        }
        
        Ok(vec![ParsedLine {
            label: None,
            mnemonic: Some("ADDI".to_string()),
            operands: vec![operands[0].clone(), operands[0].clone(), "1".to_string()],
            directive: None,
            directive_args: Vec::new(),
            line_number: 0,
            raw: String::new(),
        }])
    }
}

// DEC rd - Decrement register
struct DecInstruction;
impl VirtualInstruction for DecInstruction {
    fn name(&self) -> &str { "DEC" }
    
    fn expand(&self, operands: &[String]) -> Result<Vec<ParsedLine>, String> {
        if operands.len() != 1 {
            return Err(format!("DEC requires 1 operand, got {}", operands.len()));
        }
        
        Ok(vec![ParsedLine {
            label: None,
            mnemonic: Some("ADDI".to_string()),
            operands: vec![operands[0].clone(), operands[0].clone(), "-1".to_string()],
            directive: None,
            directive_args: Vec::new(),
            line_number: 0,
            raw: String::new(),
        }])
    }
}

// NEG rd - Negate register (two's complement)
struct NegInstruction;
impl VirtualInstruction for NegInstruction {
    fn name(&self) -> &str { "NEG" }
    
    fn expand(&self, operands: &[String]) -> Result<Vec<ParsedLine>, String> {
        if operands.len() != 1 {
            return Err(format!("NEG requires 1 operand, got {}", operands.len()));
        }
        
        Ok(vec![
            ParsedLine {
                label: None,
                mnemonic: Some("XOR".to_string()),
                operands: vec![operands[0].clone(), operands[0].clone(), "-1".to_string()],
                directive: None,
                directive_args: Vec::new(),
                line_number: 0,
                raw: String::new(),
            },
            ParsedLine {
                label: None,
                mnemonic: Some("ADDI".to_string()),
                operands: vec![operands[0].clone(), operands[0].clone(), "1".to_string()],
                directive: None,
                directive_args: Vec::new(),
                line_number: 0,
                raw: String::new(),
            },
        ])
    }
}

// NOT rd - Bitwise NOT
struct NotInstruction;
impl VirtualInstruction for NotInstruction {
    fn name(&self) -> &str { "NOT" }
    
    fn expand(&self, operands: &[String]) -> Result<Vec<ParsedLine>, String> {
        if operands.len() != 1 {
            return Err(format!("NOT requires 1 operand, got {}", operands.len()));
        }
        
        Ok(vec![ParsedLine {
            label: None,
            mnemonic: Some("XORI".to_string()),
            operands: vec![operands[0].clone(), operands[0].clone(), "-1".to_string()],
            directive: None,
            directive_args: Vec::new(),
            line_number: 0,
            raw: String::new(),
        }])
    }
}

struct SubiInstruction;
impl VirtualInstruction for SubiInstruction {
    fn name(&self) -> &str { "SUBI" }

    fn expand(&self, operands: &[String]) -> Result<Vec<ParsedLine>, String> {
        if operands.len() != 3 {
            return Err(format!("SUBI requires 3 operands, got {}", operands.len()));
        }
        
        // Assuming SUBI is a virtual instruction that subtracts an immediate value
        // We need to parse operands[2] as an integer, and 65536 - operands[2] as the immediate value
        if let Ok(value) = operands[2].parse::<i32>() {
            let immediate_value = 65536 - value;
            return Ok(vec![ParsedLine {
                label: None,
                mnemonic: Some("ADDI".to_string()),
                operands: vec![operands[0].clone(), operands[1].clone(), immediate_value.to_string()],
                directive: None,
                directive_args: Vec::new(),
                line_number: 0,
                raw: String::new(),
            }]);
        }

        Err(format!("Invalid immediate value in SUBI: {}", operands[2]))
    }
}

// API for adding custom virtual instructions
pub fn create_custom_instruction(
    name: String,
    expansion: Vec<(String, Vec<String>)>, // (mnemonic, operands)
) -> Box<dyn VirtualInstruction> {
    Box::new(CustomInstruction { name, expansion })
}

struct CustomInstruction {
    name: String,
    expansion: Vec<(String, Vec<String>)>,
}

impl VirtualInstruction for CustomInstruction {
    fn name(&self) -> &str {
        &self.name
    }
    
    fn expand(&self, _operands: &[String]) -> Result<Vec<ParsedLine>, String> {
        let mut result = Vec::new();
        
        for (mnemonic, operands) in &self.expansion {
            result.push(ParsedLine {
                label: None,
                mnemonic: Some(mnemonic.clone()),
                operands: operands.clone(),
                directive: None,
                directive_args: Vec::new(),
                line_number: 0,
                raw: String::new(),
            });
        }
        
        Ok(result)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_move_expansion() {
        let mov = MoveInstruction;
        let expanded = mov.expand(&["R3".to_string(), "R4".to_string()]).unwrap();
        
        assert_eq!(expanded.len(), 1);
        assert_eq!(expanded[0].mnemonic.as_ref().unwrap(), "ADD");
        assert_eq!(expanded[0].operands, vec!["R3", "R4", "R0"]);
    }

    #[test]
    fn test_push_expansion() {
        let push = PushInstruction;
        let expanded = push.expand(&["R3".to_string()]).unwrap();
        
        assert_eq!(expanded.len(), 2);
        assert_eq!(expanded[0].mnemonic.as_ref().unwrap(), "ADDI");
        assert_eq!(expanded[1].mnemonic.as_ref().unwrap(), "STORE");
    }

    #[test]
    fn test_inc_expansion() {
        let inc = IncInstruction;
        let expanded = inc.expand(&["R5".to_string()]).unwrap();
        
        assert_eq!(expanded.len(), 1);
        assert_eq!(expanded[0].mnemonic.as_ref().unwrap(), "ADDI");
        assert_eq!(expanded[0].operands, vec!["R5", "R5", "1"]);
    }

    #[test]
    fn test_ret_expansion() {
        let ret = RetInstruction;
        let expanded = ret.expand(&[]).unwrap();

        assert_eq!(expanded.len(), 1);
        assert_eq!(expanded[0].mnemonic.as_ref().unwrap(), "JALR");
        assert_eq!(expanded[0].operands, vec!["R0", "RAB", "RA"]);
    }
}