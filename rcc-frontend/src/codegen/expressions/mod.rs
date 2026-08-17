//! Expression code generation modules

mod literals;
mod identifiers;
mod binary_ops;
mod unary_ops;
mod pointer_ops;
mod function_calls;
mod assignments;
mod misc_ops;
mod aggregate_init;

pub use literals::generate_string_literal;
pub use identifiers::generate_identifier;
pub use binary_ops::{generate_binary_operation, generate_compound_assignment};
pub use unary_ops::{generate_unary_operation, generate_lvalue_address};
pub use pointer_ops::{generate_pointer_arithmetic, generate_pointer_difference, generate_array_index, generate_pointer_compound_assignment};
pub use function_calls::generate_function_call;
pub use assignments::{generate_assignment, copy_struct};
pub use misc_ops::{generate_sizeof_expr, generate_sizeof_type, generate_array_initializer};
pub use aggregate_init::store_initializer;

use super::errors::CodegenError;
use super::types::convert_type;
use super::VarInfo;
use crate::ir::{IrBuilder, Module, Value, FatPointer};
use crate::typed_ast::TypedExpr;
use crate::types::{Type, BankTag};
use crate::CompilerError;
use std::collections::HashMap;

// Helper function to check if a type is an integer type
fn is_integer_type(ty: &Type) -> bool {
    matches!(ty, 
        Type::Bool | Type::Char | Type::SignedChar | Type::UnsignedChar |
        Type::Short | Type::UnsignedShort | Type::Int | Type::UnsignedInt |
        Type::Long | Type::UnsignedLong | Type::Enum { .. }
    )
}

// Helper function for convert_type with default location
fn convert_type_default(ast_type: &crate::types::Type) -> Result<crate::ir::IrType, CompilerError> {
    convert_type(ast_type, rcc_common::SourceLocation::new_simple(0, 0))
}

/// Typed expression generator context
pub struct TypedExpressionGenerator<'a> {
    pub builder: &'a mut IrBuilder,
    pub module: &'a mut Module,
    pub variables: &'a HashMap<String, VarInfo>,
    pub array_variables: &'a std::collections::HashSet<String>,
    pub parameter_variables: &'a std::collections::HashSet<String>,
    pub string_literals: &'a mut HashMap<String, String>,
    pub next_string_id: &'a mut u32,
}

impl<'a> TypedExpressionGenerator<'a> {
    /// Generate IR for a typed expression
    pub fn generate(&mut self, expr: &TypedExpr) -> Result<Value, CompilerError> {
        match expr {
            TypedExpr::IntLiteral { value, .. } => Ok(Value::Constant(*value)),
            
            TypedExpr::CharLiteral { value, .. } => Ok(Value::Constant(*value as i64)),
            
            TypedExpr::StringLiteral { value, .. } => literals::generate_string_literal(self, value),
            
            TypedExpr::Variable { name, .. } => identifiers::generate_identifier(self, name),
            
            TypedExpr::Binary {
                op,
                left,
                right,
                expr_type,
            } => binary_ops::generate_binary_operation(self, *op, left, right, expr_type),
            
            TypedExpr::PointerArithmetic {
                ptr,
                offset,
                elem_type,
                is_add,
                expr_type,
            } => pointer_ops::generate_pointer_arithmetic(self, ptr, offset, elem_type, *is_add, expr_type),
            
            TypedExpr::PointerDifference {
                left,
                right,
                elem_type,
                ..
            } => pointer_ops::generate_pointer_difference(self, left, right, elem_type),
            
            TypedExpr::ArrayIndex {
                array,
                index,
                elem_type,
                ..
            } => pointer_ops::generate_array_index(self, array, index, elem_type),
            
            TypedExpr::Unary {
                op,
                operand,
                expr_type,
            } => unary_ops::generate_unary_operation(self, *op, operand, expr_type),
            
            TypedExpr::Call {
                function,
                arguments,
                expr_type,
                stack_args_from,
            } => function_calls::generate_function_call(self, function, arguments, expr_type, *stack_args_from),
            
            TypedExpr::Cast {
                operand,
                target_type,
                ..
            } => {
                // Generate code for the operand
                let operand_val = self.generate(operand)?;
                let source_type = operand.get_type();
                
                // Handle different cast scenarios
                match (&source_type, target_type) {
                    // Pointer to pointer cast (including void*)
                    (Type::Pointer { .. }, Type::Pointer { .. }) => {
                        // Pointer-to-pointer casts preserve the value
                        // Fat pointer bank tags are preserved during cast
                        Ok(operand_val)
                    }
                    
                    // Integer to pointer cast
                    (source, Type::Pointer { .. }) if is_integer_type(source) => {
                        // Create a fat pointer from an integer value
                        // Check if this is a NULL pointer (literal 0)
                        let bank_tag = match &operand_val {
                            Value::Constant(0) => BankTag::Null,  // NULL pointer
                            _ => BankTag::Heap(0),  // Integer-to-pointer (MMIO, etc.) uses bank 0
                        };
                        Ok(Value::FatPtr(FatPointer {
                            addr: Box::new(operand_val),
                            bank: bank_tag,
                        }))
                    }
                    
                    // Pointer to integer cast  
                    (Type::Pointer { .. }, target) if is_integer_type(target) => {
                        // Extract address component from fat pointer
                        match operand_val {
                            Value::FatPtr(ref fp) => {
                                // Return just the address component
                                Ok(*fp.addr.clone())
                            }
                            _ => {
                                // If it's not a FatPtr (shouldn't happen), pass through
                                Ok(operand_val)
                            }
                        }
                    }
                    
                    // Integer / pointer to _Bool: nonzero becomes 1
                    (source, Type::Bool) if is_integer_type(source) || matches!(source, Type::Pointer { .. }) => {
                        let operand_val = match operand_val {
                            Value::FatPtr(fp) => *fp.addr,
                            other => other,
                        };
                        let temp = self.builder.build_binary(
                            crate::ir::IrBinaryOp::Ne,
                            operand_val,
                            Value::Constant(0),
                            crate::ir::IrType::I16,
                        ).map_err(|e| CodegenError::InternalError {
                            message: e,
                            location: rcc_common::SourceLocation::new_simple(0, 0),
                        })?;
                        Ok(Value::Temp(temp))
                    }

                    // Integer to integer cast
                    (source, target) if is_integer_type(source) && is_integer_type(target) => {
                        // For now, pass through the value since our VM uses 16-bit cells uniformly
                        // In a full implementation, we would:
                        // - Sign extend when casting signed to larger type
                        // - Zero extend when casting unsigned to larger type  
                        // - Truncate when casting to smaller type
                        // Since Ripple VM uses 16-bit cells for most integer types,
                        // many casts are no-ops at the IR level
                        Ok(operand_val)
                    }
                    
                    // Void cast (discarding value)
                    (_, Type::Void) => {
                        // Cast to void means discard the value
                        // Return a dummy value since void has no representation
                        Ok(Value::Constant(0))
                    }
                    
                    // Array to pointer decay (implicit cast)
                    (Type::Array { element_type, .. }, Type::Pointer { target, .. })
                        if **element_type == **target => {
                        // Array decays to pointer to first element
                        Ok(operand_val)
                    }
                    
                    _ => {
                        Err(CodegenError::UnsupportedConstruct {
                            construct: format!("cast from {source_type:?} to {target_type:?}"),
                            location: rcc_common::SourceLocation::new_simple(0, 0),
                        }
                        .into())
                    }
                }
            }
            
            TypedExpr::Assignment { lhs, rhs, .. } => assignments::generate_assignment(self, lhs, rhs),
            
            TypedExpr::CompoundAssignment {
                op,
                lhs,
                rhs,
                is_pointer,
                ..
            } => {
                if *is_pointer {
                    pointer_ops::generate_pointer_compound_assignment(self, *op, lhs, rhs)
                } else {
                    binary_ops::generate_compound_assignment(self, *op, lhs, rhs)
                }
            }
            
            TypedExpr::SizeofExpr { operand, .. } => misc_ops::generate_sizeof_expr(self, operand),
            
            TypedExpr::SizeofType { target_type, .. } => misc_ops::generate_sizeof_type(self, target_type),
            
            TypedExpr::ArrayInitializer { elements, .. } => misc_ops::generate_array_initializer(self, elements),
            
            TypedExpr::CompoundLiteral { initializer, expr_type } => {
                // Compound literals create anonymous temporary objects with automatic storage duration
                let alloc_type = match expr_type {
                    Type::Array { element_type, size } => {
                        let array_size = size.unwrap_or(initializer.len() as u64);
                        let elem_ir_type = convert_type_default(element_type)?;
                        crate::ir::IrType::Array {
                            size: array_size,
                            element_type: Box::new(elem_ir_type),
                        }
                    }
                    Type::Struct { .. } | Type::Union { .. } if expr_type.is_aggregate() => {
                        let struct_size = expr_type.size_in_words()
                            .ok_or_else(|| CodegenError::InternalError {
                                message: "Cannot determine aggregate size for compound literal".to_string(),
                                location: rcc_common::SourceLocation::new_simple(0, 0),
                            })?;
                        crate::ir::IrType::Array {
                            size: struct_size,
                            element_type: Box::new(crate::ir::IrType::I16),
                        }
                    }
                    _ => convert_type_default(expr_type)?,
                };

                let object_ptr = self.builder.build_alloca(alloc_type, None)
                    .map_err(|e| CodegenError::InternalError {
                        message: format!("Failed to allocate space for compound literal: {e}"),
                        location: rcc_common::SourceLocation::new_simple(0, 0),
                    })?;

                let init_expr = TypedExpr::ArrayInitializer {
                    elements: initializer.clone(),
                    expr_type: expr_type.clone(),
                };
                aggregate_init::store_initializer(self, object_ptr.clone(), expr_type, &init_expr)?;
                Ok(object_ptr)
            }
            
            TypedExpr::MemberAccess { 
                object, 
                member: _,
                offset, 
                is_pointer, 
                expr_type 
            } => {
                // Following POINTER_ARITHMETIC_ROADMAP.md Task 2.3:
                // Struct field access MUST be converted to GEP
                
                // Get the struct type from the object
                let struct_type = if *is_pointer {
                    // For -> operator, object type is Pointer<Struct>
                    match object.get_type() {
                        Type::Pointer { target, .. } => &**target,
                        _ => object.get_type(), // Shouldn't happen, but fallback
                    }
                } else {
                    // For . operator, object type is Struct
                    object.get_type()
                };
                
                // Convert struct type to IR type
                let struct_ir_type = convert_type_default(struct_type)?;
                
                // Pointers (`->`) and struct values (`.`) both generate as fat
                // pointers: variables, call results, compound literals, nested
                // aggregate members, and struct-typed ternaries.
                let struct_ptr = self.generate(object)?;
                
                // Field offset is a compile-time constant (in words)
                // Convert field type to IR type
                let field_type_ir = convert_type_default(expr_type)?;
                
                // Use build_struct_field_gep for proper struct field access
                // This correctly sets source_element_type to the struct type
                let field_ptr = self.builder.build_struct_field_gep(
                    struct_ptr,
                    *offset as i64,
                    struct_ir_type,
                    field_type_ir.clone()
                )?;
                
                // Check if the field is an array type, struct type, or pointer type
                // Arrays and structs should decay to pointers when accessed (not loaded)
                // Pointers should be returned as FatPtr for further operations
                if matches!(expr_type, Type::Array { .. }) || expr_type.is_aggregate() {
                    // Aggregates decay to pointers when accessed as values
                    Ok(field_ptr)
                } else if let Type::Pointer { target, .. } = expr_type {
                    // For pointer fields, load the value as a fat pointer
                    // The backend's load instruction will handle loading both components
                    // Convert the pointee type to IR type for the FatPtr
                    let pointee_ir_type = convert_type(target, rcc_common::SourceLocation::new_simple(0, 0))?;
                    let ptr_load_type = crate::ir::IrType::FatPtr(Box::new(pointee_ir_type));
                    let temp_id = self.builder.build_load(field_ptr, ptr_load_type)
                        .map_err(|e| CodegenError::InternalError {
                            message: e,
                            location: rcc_common::SourceLocation::new_simple(0, 0),
                        })?;
                    // The loaded pointer has a dynamically loaded bank, so mark as Mixed
                    Ok(Value::FatPtr(FatPointer {
                        addr: Box::new(Value::Temp(temp_id)),
                        bank: BankTag::Mixed,
                    }))
                } else {
                    // For other fields (scalars), load the value normally
                    let temp_id = self.builder.build_load(field_ptr, field_type_ir)
                        .map_err(|e| CodegenError::InternalError {
                            message: e,
                            location: rcc_common::SourceLocation::new_simple(0, 0),
                        })?;
                    Ok(Value::Temp(temp_id))
                }
            }
            
            TypedExpr::Conditional { condition, then_expr, else_expr, expr_type } => {
                // Evaluate condition
                let cond_value = self.generate(condition)?;
                
                // Arrays in a ternary decay to pointers (C99).
                let is_aggregate = expr_type.is_aggregate();
                let value_type = match expr_type {
                    Type::Array { element_type, .. } => Type::Pointer {
                        target: element_type.clone(),
                        bank: None,
                    },
                    other => other.clone(),
                };
                let ir_type = convert_type_default(&value_type)?;
                
                // We need to transform the ternary into if-else with a temporary variable
                // This is done by creating a temporary variable to hold the result
                
                // Allocate space for the result value
                let result_ptr = self.builder.build_alloca(ir_type.clone(), None)
                    .map_err(|e| CodegenError::InternalError {
                        message: format!("Failed to allocate temporary for ternary: {e}"),
                        location: rcc_common::SourceLocation::new_simple(0, 0),
                    })?;
                
                // Create labels for control flow
                let then_label = self.builder.new_label();
                let else_label = self.builder.new_label();
                let end_label = self.builder.new_label();
                
                // Branch based on condition
                self.builder.build_branch_cond(cond_value, then_label, else_label)
                    .map_err(|e| CodegenError::InternalError {
                        message: format!("Failed to generate conditional branch: {e}"),
                        location: rcc_common::SourceLocation::new_simple(0, 0),
                    })?;
                
                // Generate then branch
                self.builder.create_block(then_label)
                    .map_err(|e| CodegenError::InternalError {
                        message: format!("Failed to create then block: {e}"),
                        location: rcc_common::SourceLocation::new_simple(0, 0),
                    })?;
                let then_value = self.generate(then_expr)?;
                if is_aggregate {
                    assignments::copy_struct(self, then_value, result_ptr.clone(), expr_type)?;
                } else {
                    self.builder.build_store(then_value, result_ptr.clone())
                        .map_err(|e| CodegenError::InternalError {
                            message: format!("Failed to store then value: {e}"),
                            location: rcc_common::SourceLocation::new_simple(0, 0),
                        })?;
                }
                // Only create branch if block doesn't already have a terminator
                if !self.builder.current_block_has_terminator() {
                    self.builder.build_branch(end_label)
                        .map_err(|e| CodegenError::InternalError {
                            message: format!("Failed to branch to end: {e}"),
                            location: rcc_common::SourceLocation::new_simple(0, 0),
                        })?;
                }
                
                // Generate else branch
                self.builder.create_block(else_label)
                    .map_err(|e| CodegenError::InternalError {
                        message: format!("Failed to create else block: {e}"),
                        location: rcc_common::SourceLocation::new_simple(0, 0),
                    })?;
                let else_value = self.generate(else_expr)?;
                if is_aggregate {
                    assignments::copy_struct(self, else_value, result_ptr.clone(), expr_type)?;
                } else {
                    self.builder.build_store(else_value, result_ptr.clone())
                        .map_err(|e| CodegenError::InternalError {
                            message: format!("Failed to store else value: {e}"),
                            location: rcc_common::SourceLocation::new_simple(0, 0),
                        })?;
                }
                // Only create branch if block doesn't already have a terminator
                if !self.builder.current_block_has_terminator() {
                    self.builder.build_branch(end_label)
                        .map_err(|e| CodegenError::InternalError {
                            message: format!("Failed to branch to end: {e}"),
                            location: rcc_common::SourceLocation::new_simple(0, 0),
                        })?;
                }
                
                // Create end block and load the result
                self.builder.create_block(end_label)
                    .map_err(|e| CodegenError::InternalError {
                        message: format!("Failed to create end block: {e}"),
                        location: rcc_common::SourceLocation::new_simple(0, 0),
                    })?;
                
                if is_aggregate {
                    // Aggregates are used as pointers (assignment, member access, byval args)
                    Ok(result_ptr)
                } else {
                    let result = self.builder.build_load(result_ptr, ir_type)
                        .map_err(|e| CodegenError::InternalError {
                            message: format!("Failed to load result: {e}"),
                            location: rcc_common::SourceLocation::new_simple(0, 0),
                        })?;
                    Ok(Value::Temp(result))
                }
            }

            TypedExpr::VaStart { ap, .. } => {
                let ap_addr = unary_ops::generate_lvalue_address(self, ap)?;
                self.builder.build_intrinsic("va_start", vec![ap_addr], crate::ir::IrType::Void)
                    .map_err(|e| CodegenError::InternalError {
                        message: format!("Failed to emit va_start: {e}"),
                        location: rcc_common::SourceLocation::new_simple(0, 0),
                    })?;
                Ok(Value::Constant(0))
            }

            TypedExpr::VaArg { ap, arg_type, expr_type } => {
                let ap_addr = unary_ops::generate_lvalue_address(self, ap)?;
                let ir_type = convert_type_default(arg_type)?;
                let result = self.builder.build_intrinsic("va_arg", vec![ap_addr], ir_type.clone())
                    .map_err(|e| CodegenError::InternalError {
                        message: format!("Failed to emit va_arg: {e}"),
                        location: rcc_common::SourceLocation::new_simple(0, 0),
                    })?;
                match result {
                    Some(temp_id) => {
                        if matches!(expr_type, Type::Pointer { .. }) {
                            Ok(Value::FatPtr(FatPointer {
                                addr: Box::new(Value::Temp(temp_id)),
                                bank: BankTag::Mixed,
                            }))
                        } else {
                            Ok(Value::Temp(temp_id))
                        }
                    }
                    None => Ok(Value::Constant(0)),
                }
            }
        }
    }
}