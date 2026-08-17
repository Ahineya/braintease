//! Binary expression operations and type checking

use std::cell::RefCell;
use std::rc::Rc;
use crate::ast::*;
use crate::semantic::errors::SemanticError;
use crate::semantic::types::TypeAnalyzer;
use crate::Type;
use rcc_common::{CompilerError, SourceLocation};

pub struct BinaryOperationAnalyzer {
    type_analyzer: Rc<RefCell<TypeAnalyzer>>
}

impl BinaryOperationAnalyzer {
    pub fn new(type_analyzer: Rc<RefCell<TypeAnalyzer>>) -> Self {
        Self { type_analyzer }
    }

    /// Analyze binary operation and return result type
    pub fn analyze(
        &self,
        op: BinaryOp,
        left: &Expression,
        right: &Expression,
    ) -> Result<Type, CompilerError> {
        let left_type = left.expr_type.as_ref().unwrap_or(&Type::Error);
        let right_type = right.expr_type.as_ref().unwrap_or(&Type::Error);

        match op {
            // Arithmetic operations
            BinaryOp::Add | BinaryOp::Sub => {
                self.analyze_additive_operation(op, left_type, right_type, &left.span.start)
            }

            BinaryOp::Mul | BinaryOp::Div | BinaryOp::Mod => {
                
                if self.type_analyzer.borrow().is_integer(left_type) && self.type_analyzer.borrow().is_integer(right_type) {
                    Ok(self.type_analyzer.borrow().arithmetic_result_type(left_type, right_type))
                } else {
                    Err(SemanticError::InvalidOperation {
                        operation: format!("{op}"),
                        operand_type: left_type.clone(),
                        location: left.span.start.clone(),
                    }
                    .into())
                }
            }

            // Bitwise operations
            | BinaryOp::LeftShift
            | BinaryOp::RightShift => {
                // C99 6.5.7: the result type is that of the promoted left operand.
                if self.type_analyzer.borrow().is_integer(left_type) && self.type_analyzer.borrow().is_integer(right_type) {
                    Ok(left_type.clone())
                } else {
                    Err(SemanticError::InvalidOperation {
                        operation: format!("{op}"),
                        operand_type: left_type.clone(),
                        location: left.span.start.clone(),
                    }
                    .into())
                }
            }

            BinaryOp::BitAnd
            | BinaryOp::BitOr
            | BinaryOp::BitXor => {
                if self.type_analyzer.borrow().is_integer(left_type) && self.type_analyzer.borrow().is_integer(right_type) {
                    Ok(self.type_analyzer.borrow().arithmetic_result_type(left_type, right_type))
                } else {
                    Err(SemanticError::InvalidOperation {
                        operation: format!("{op}"),
                        operand_type: left_type.clone(),
                        location: left.span.start.clone(),
                    }
                    .into())
                }
            }

            // Comparison operations
            BinaryOp::Less
            | BinaryOp::Greater
            | BinaryOp::LessEqual
            | BinaryOp::GreaterEqual
            | BinaryOp::Equal
            | BinaryOp::NotEqual => {
                // Comparisons return int (boolean)
                Ok(Type::Int)
            }

            // Logical operations
            BinaryOp::LogicalAnd | BinaryOp::LogicalOr => {
                Ok(Type::Int) // Logical operations return int
            }

            BinaryOp::Comma => {
                // Comma operator: evaluate left, result is the right operand
                Ok(right_type.clone())
            }

            // Assignment operations
            BinaryOp::Assign => {
                // Check if left is an lvalue
                if !TypeAnalyzer::is_lvalue(left) {
                    return Err(SemanticError::InvalidLvalue {
                        location: left.span.start.clone(),
                    }
                    .into());
                }

                // Check type compatibility with typedef awareness
                if !self.type_analyzer.borrow().is_assignable(left_type, right_type) {
                    return Err(SemanticError::TypeMismatch {
                        expected: left_type.clone(),
                        found: right_type.clone(),
                        location: right.span.start.clone(),
                    }
                    .into());
                }

                Ok(left_type.clone())
            }

            // Compound assignment operations
            BinaryOp::AddAssign | BinaryOp::SubAssign => {
                // Check if left is an lvalue
                if !TypeAnalyzer::is_lvalue(left) {
                    return Err(SemanticError::InvalidLvalue {
                        location: left.span.start.clone(),
                    }
                    .into());
                }

                // For pointers, += and -= work like pointer arithmetic
                if self.type_analyzer.borrow().is_pointer(left_type) && self.type_analyzer.borrow().is_integer(right_type) {
                    Ok(left_type.clone())
                } else if self.type_analyzer.borrow().is_integer(left_type) && self.type_analyzer.borrow().is_integer(right_type) {
                    Ok(left_type.clone())
                } else {
                    Err(SemanticError::TypeMismatch {
                        expected: left_type.clone(),
                        found: right_type.clone(),
                        location: right.span.start.clone(),
                    }
                    .into())
                }
            }

            BinaryOp::MulAssign
            | BinaryOp::DivAssign
            | BinaryOp::ModAssign
            | BinaryOp::BitAndAssign
            | BinaryOp::BitOrAssign
            | BinaryOp::BitXorAssign
            | BinaryOp::LeftShiftAssign
            | BinaryOp::RightShiftAssign => {
                // Check if left is an lvalue
                if !TypeAnalyzer::is_lvalue(left) {
                    return Err(SemanticError::InvalidLvalue {
                        location: left.span.start.clone(),
                    }
                    .into());
                }

                // These operations only work on integers
                if self.type_analyzer.borrow().is_integer(left_type) && self.type_analyzer.borrow().is_integer(right_type) {
                    Ok(left_type.clone())
                } else {
                    Err(SemanticError::TypeMismatch {
                        expected: left_type.clone(),
                        found: right_type.clone(),
                        location: right.span.start.clone(),
                    }
                    .into())
                }
            }

            BinaryOp::Index => {
                // Array indexing: arr[index]
                // First resolve typedef if needed
                let resolved_left = self.type_analyzer.borrow().resolve_type(left_type);
                
                // Arrays decay to pointers for indexing
                let array_type = if let Type::Array { element_type, .. } = &resolved_left {
                    Type::Pointer {
                        target: element_type.clone(),
                        bank: None, // Arrays are typically on stack or global
                    }
                } else {
                    resolved_left.clone()
                };

                if self.type_analyzer.borrow().is_pointer(&array_type) && self.type_analyzer.borrow().is_integer(right_type) {
                    if let Some(target_type) = self.type_analyzer.borrow().pointer_target(&array_type) {
                        Ok(target_type)
                    } else {
                        Ok(Type::Error)
                    }
                } else {
                    Err(SemanticError::InvalidOperation {
                        operation: "array indexing".to_string(),
                        operand_type: left_type.clone(),
                        location: left.span.start.clone(),
                    }
                    .into())
                }
            }
        }
    }

    /// Analyze additive operations (+ and -) with proper pointer arithmetic handling
    fn analyze_additive_operation(
        &self,
        op: BinaryOp,
        left_type: &Type,
        right_type: &Type,
        location: &SourceLocation,
    ) -> Result<Type, CompilerError> {
        // CRITICAL: Handle array decay - arrays decay to pointers in arithmetic contexts
        // When p[0] returns an array, it should decay to a pointer for arithmetic
        let left_decayed = if let Type::Array { element_type, .. } = left_type {
            Type::Pointer {
                target: element_type.clone(),
                bank: None,
            }
        } else {
            left_type.clone()
        };
        
        let right_decayed = if let Type::Array { element_type, .. } = right_type {
            Type::Pointer {
                target: element_type.clone(),
                bank: None,
            }
        } else {
            right_type.clone()
        };
        
        // Case 1: Integer + Integer
        if self.type_analyzer.borrow().is_integer(&left_decayed) && self.type_analyzer.borrow().is_integer(&right_decayed) {
            return Ok(self.type_analyzer.borrow().arithmetic_result_type(&left_decayed, &right_decayed));
        }

        // Case 2: Pointer + Integer (after array decay)
        if self.type_analyzer.borrow().is_pointer(&left_decayed) && self.type_analyzer.borrow().is_integer(&right_decayed) {
            // Verify the pointer target has a known size
            if let Some(target) = self.type_analyzer.borrow().pointer_target(&left_decayed) {
                if target.size_in_words().is_none() {
                    return Err(SemanticError::InvalidOperation {
                        operation: "pointer arithmetic on incomplete type".to_string(),
                        operand_type: left_decayed.clone(),
                        location: location.clone(),
                    }
                    .into());
                }
                
                // CRITICAL FIX: If pointer points to an array, arithmetic on it
                // should yield a pointer to the array's element type, not to the array itself.
                // For example: int (*p)[3] + 1 should give int*, not int (*)[3]
                if let Type::Array { element_type, .. } = &target {
                    return Ok(Type::Pointer {
                        target: element_type.clone(),
                        bank: if let Type::Pointer { bank, .. } = &left_decayed {
                            bank.clone()
                        } else {
                            None
                        }
                    });
                }
            }
            return Ok(left_decayed.clone());
        }

        // Case 3: Integer + Pointer (only for Add, commutative, after array decay)
        if self.type_analyzer.borrow().is_integer(&left_decayed) && self.type_analyzer.borrow().is_pointer(&right_decayed) && op == BinaryOp::Add {
            // Verify the pointer target has a known size
            if let Some(target) = self.type_analyzer.borrow().pointer_target(&right_decayed) {
                if target.size_in_words().is_none() {
                    return Err(SemanticError::InvalidOperation {
                        operation: "pointer arithmetic on incomplete type".to_string(),
                        operand_type: right_decayed.clone(),
                        location: location.clone(),
                    }
                    .into());
                }
                
                // CRITICAL FIX: If pointer points to an array, arithmetic on it
                // should yield a pointer to the array's element type, not to the array itself.
                // For example: int (*p)[3] + 1 should give int*, not int (*)[3]
                if let Type::Array { element_type, .. } = &target {
                    return Ok(Type::Pointer {
                        target: element_type.clone(),
                        bank: if let Type::Pointer { bank, .. } = &right_decayed {
                            bank.clone()
                        } else {
                            None
                        }
                    });
                }
            }
            return Ok(right_decayed.clone());
        }

        // Case 4: Pointer - Pointer (only for Sub, after array decay)
        if self.type_analyzer.borrow().is_pointer(&left_decayed) && self.type_analyzer.borrow().is_pointer(&right_decayed) && op == BinaryOp::Sub {
            // Both pointers must point to compatible types
            let left_target = self.type_analyzer.borrow().pointer_target(&left_decayed);
            let right_target = self.type_analyzer.borrow().pointer_target(&right_decayed);

            if let (Some(left_elem), Some(right_elem)) = (left_target, right_target) {
                // Check if element types are compatible
                if !self.are_types_compatible(&left_elem, &right_elem) {
                    return Err(SemanticError::InvalidOperation {
                        operation: "subtracting pointers to incompatible types".to_string(),
                        operand_type: left_decayed.clone(),
                        location: location.clone(),
                    }
                    .into());
                }

                // Verify the types have known sizes
                if left_elem.size_in_words().is_none() {
                    return Err(SemanticError::InvalidOperation {
                        operation: "pointer difference on incomplete type".to_string(),
                        operand_type: left_decayed.clone(),
                        location: location.clone(),
                    }
                    .into());
                }
            }

            // Pointer difference returns ptrdiff_t (we use int)
            return Ok(Type::Int);
        }

        Err(SemanticError::InvalidOperation {
            operation: format!("{op}"),
            operand_type: left_decayed.clone(),
            location: location.clone(),
        }
        .into())
    }

    /// Check if two types are compatible (for pointer operations)
    fn are_types_compatible(&self, left: &Type, right: &Type) -> bool {
        // Use typedef-aware compatibility check
        self.type_analyzer.borrow().is_assignable(left, right)
    }
}