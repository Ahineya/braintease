//! Unary expression operations and type checking

use std::cell::RefCell;
use std::rc::Rc;
use crate::ast::*;
use crate::semantic::errors::SemanticError;
use crate::semantic::types::TypeAnalyzer;
use crate::{BankTag, Type};
use rcc_common::{CompilerError, StorageClass as CommonStorageClass};

pub struct UnaryOperationAnalyzer {
    pub type_analyzer: Rc<RefCell<TypeAnalyzer>>
}

impl UnaryOperationAnalyzer {
    pub fn new(type_analyzer: Rc<RefCell<TypeAnalyzer>>) -> Self {
        Self { type_analyzer }
    }
    
    /// Analyze unary operation and return result type
    pub fn analyze(
        &self,
        op: UnaryOp,
        operand: &Expression,
    ) -> Result<Type, CompilerError> {
        let operand_type = operand.expr_type.as_ref().unwrap_or(&Type::Error);

        match op {
            UnaryOp::Plus | UnaryOp::Minus => {
                if self.type_analyzer.borrow().is_arithmetic(operand_type) {
                    Ok(operand_type.clone())
                } else {
                    Err(SemanticError::InvalidOperation {
                        operation: format!("{op}"),
                        operand_type: operand_type.clone(),
                        location: operand.span.start.clone(),
                    }
                    .into())
                }
            }

            UnaryOp::BitNot => {
                if self.type_analyzer.borrow().is_integer(operand_type) {
                    Ok(operand_type.clone())
                } else {
                    Err(SemanticError::InvalidOperation {
                        operation: "bitwise NOT".to_string(),
                        operand_type: operand_type.clone(),
                        location: operand.span.start.clone(),
                    }
                    .into())
                }
            }

            UnaryOp::LogicalNot => {
                Ok(Type::Int) // Logical NOT always returns int
            }

            UnaryOp::Dereference => {
                if let Some(target_type) = self.type_analyzer.borrow().pointer_target(operand_type) {
                    Ok(target_type)
                } else {
                    Err(SemanticError::InvalidOperation {
                        operation: "dereference".to_string(),
                        operand_type: operand_type.clone(),
                        location: operand.span.start.clone(),
                    }
                    .into())
                }
            }

            UnaryOp::AddressOf => {
                if TypeAnalyzer::is_lvalue(operand) {
                    // Determine bank based on operand
                    let bank = self.determine_bank_for_address_of(operand);
                    Ok(Type::Pointer {
                        target: Box::new(operand_type.clone()),
                        bank,
                    })
                } else {
                    Err(SemanticError::InvalidOperation {
                        operation: "address-of".to_string(),
                        operand_type: operand_type.clone(),
                        location: operand.span.start.clone(),
                    }
                    .into())
                }
            }

            UnaryOp::PreIncrement
            | UnaryOp::PostIncrement
            | UnaryOp::PreDecrement
            | UnaryOp::PostDecrement => {
                if !TypeAnalyzer::is_lvalue(operand) {
                    return Err(SemanticError::InvalidLvalue {
                        location: operand.span.start.clone(),
                    }
                    .into());
                }

                if self.type_analyzer.borrow().is_arithmetic(operand_type) || self.type_analyzer.borrow().is_pointer(operand_type) {
                    Ok(operand_type.clone())
                } else {
                    Err(SemanticError::InvalidOperation {
                        operation: format!("{op}"),
                        operand_type: operand_type.clone(),
                        location: operand.span.start.clone(),
                    }
                    .into())
                }
            }

            UnaryOp::Sizeof => {
                Ok(Type::UnsignedInt) // sizeof yields size_t (unsigned int)
            }
        }
    }

    /// Determine the bank tag for address-of operations
    fn determine_bank_for_address_of(
        &self,
        operand: &Expression,
    ) -> Option<BankTag> {
        match &operand.kind {
            ExpressionKind::Identifier { symbol_id, .. } => {
                if let Some(id) = symbol_id {
                    if let Some(symbol) = self.type_analyzer.borrow().symbol_table.borrow().get_symbol(*id) {
                        // Local variables are on the stack
                        if matches!(
                            symbol.storage_class,
                            CommonStorageClass::Auto | CommonStorageClass::Register
                        ) {
                            return Some(BankTag::Stack);
                        }
                        // Static and extern variables are global
                        if matches!(
                            symbol.storage_class,
                            CommonStorageClass::Static | CommonStorageClass::Extern
                        ) {
                            return Some(BankTag::Global);
                        }
                    }
                }
                None
            }
            ExpressionKind::Member { .. } => {
                // For struct members, inherit the bank from the struct
                None // We'll need more context to determine this
            }
            ExpressionKind::Binary {
                op: BinaryOp::Index,
                left,
                ..
            } => {
                // Array indexing inherits bank from array
                if let Some(arr_type) = &left.expr_type {
                    if let Type::Pointer { bank, .. } = arr_type {
                        return *bank;
                    }
                }
                None
            }
            ExpressionKind::Unary {
                op: UnaryOp::Dereference,
                operand,
            } => {
                // Dereferencing a pointer - bank depends on the pointer's bank
                if let Some(ptr_type) = &operand.expr_type {
                    if let Type::Pointer { bank, .. } = ptr_type {
                        return *bank;
                    }
                }
                None
            }
            _ => None,
        }
    }
}