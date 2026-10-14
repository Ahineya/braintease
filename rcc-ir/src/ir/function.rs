//! Function Definitions
//! 
//! Defines IR functions with their parameters, blocks, and metadata.

use rcc_common::{TempId, LabelId, SymbolId, SourceLocation};
use serde::{Deserialize, Serialize};
use crate::ir::{BasicBlock, IrType};

/// Function in IR
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct Function {
    pub name: String,
    pub return_type: IrType,
    pub parameters: Vec<(TempId, IrType)>,
    pub blocks: Vec<BasicBlock>,
    pub is_external: bool,
    pub is_vararg: bool,
    
    // For code generation
    pub symbol_id: Option<SymbolId>,
    pub source_location: Option<SourceLocation>,
}

impl Function {
    pub fn new(name: String, return_type: IrType) -> Self {
        Self {
            name,
            return_type,
            parameters: Vec::new(),
            blocks: Vec::new(),
            is_external: false,
            is_vararg: false,
            symbol_id: None,
            source_location: None,
        }
    }
    
    pub fn add_parameter(&mut self, param_id: TempId, param_type: IrType) {
        self.parameters.push((param_id, param_type));
    }
    
    pub fn add_block(&mut self, block: BasicBlock) {
        self.blocks.push(block);
    }
    
    pub fn get_block(&self, id: LabelId) -> Option<&BasicBlock> {
        self.blocks.iter().find(|b| b.id == id)
    }
    
    pub fn get_block_mut(&mut self, id: LabelId) -> Option<&mut BasicBlock> {
        self.blocks.iter_mut().find(|b| b.id == id)
    }
    
    pub fn entry_block(&self) -> Option<&BasicBlock> {
        self.blocks.first()
    }
    
    pub fn entry_block_mut(&mut self) -> Option<&mut BasicBlock> {
        self.blocks.first_mut()
    }
}