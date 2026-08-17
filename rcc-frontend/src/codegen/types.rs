//! Type conversion utilities

use crate::types::Type;
use crate::typed_ast::TypedExpr;
use crate::ir::IrType;
use rcc_common::SourceLocation;
use crate::CompilerError;
use super::errors::CodegenError;

/// Complete an incomplete type using information from its initializer
/// 
/// This function handles cases where a type is declared without full information
/// (e.g., `int arr[] = {1,2,3}`) and completes it using the initializer.
/// 
/// # Examples
/// - `int arr[]` with `{1,2,3}` -> `int arr[3]`
/// - Future: struct with flexible array member
pub fn complete_type_from_initializer(
    incomplete_type: &Type,
    initializer: Option<&TypedExpr>,
) -> Type {
    match (incomplete_type, initializer) {
        // Incomplete array with array initializer - infer size from elements
        (Type::Array { element_type, size: None }, Some(TypedExpr::ArrayInitializer { elements, .. }))
        | (Type::Array { element_type, size: None }, Some(TypedExpr::CompoundLiteral { initializer: elements, .. })) => {
            Type::Array {
                element_type: element_type.clone(),
                size: Some(elements.len() as u64),
            }
        }
        
        // Future: Handle structs with flexible array members
        // (Type::Struct { fields, .. }, Some(TypedExpr::StructInitializer { .. })) => {
            // Complete the flexible array member size
        // }
        
        // No completion needed or possible
        _ => incomplete_type.clone(),
    }
}

/// Convert AST type to IR type
pub fn convert_type(ast_type: &Type, location: SourceLocation) -> Result<IrType, CompilerError> {
    match ast_type {
        Type::Void => Ok(IrType::Void),
        Type::Bool => Ok(IrType::I16),
        Type::Char | Type::SignedChar | Type::UnsignedChar => Ok(IrType::I8),
        Type::Short | Type::UnsignedShort => Ok(IrType::I16),
        Type::Int | Type::UnsignedInt => Ok(IrType::I16), // 16-bit int on Ripple
        Type::Long | Type::UnsignedLong => Ok(IrType::I32),
        Type::LongLong | Type::UnsignedLongLong => Ok(IrType::I64),
        Type::Float => Ok(IrType::I32),
        Type::Double => Ok(IrType::I64),
        Type::Pointer { target, .. } => {
            // Note: Bank information is tracked separately in codegen, not in IrType
            let target_type = convert_type(target, location)?;
            Ok(IrType::FatPtr(Box::new(target_type)))
        }
        Type::Array { element_type, size } => {
            let elem_type = convert_type(element_type, location)?;
            if let Some(size) = size {
                Ok(IrType::Array { size: *size, element_type: Box::new(elem_type) })
            } else {
                // Incomplete array type - treat as pointer for now
                Ok(IrType::FatPtr(Box::new(elem_type)))
            }
        }
        Type::Struct { name, fields } => {
            // Convert struct to IrType::Struct, preserving field type information
            let mut ir_fields = Vec::new();
            
            for field in fields {
                // Recursively convert each field type
                let field_ir_type = convert_type(&field.field_type, location.clone())?;
                ir_fields.push(field_ir_type);
            }
            
            Ok(IrType::Struct {
                name: name.clone(),
                fields: ir_fields,
                packed: false,  // C structs are not packed by default
            })
        }
        Type::Union { fields, .. } => {
            // For unions, allocate the size of the largest field
            let mut max_words = 0u64;
            for field in fields {
                if let Some(size) = field.field_type.size_in_words() {
                    max_words = max_words.max(size);
                } else {
                    return Err(CodegenError::InternalError {
                        message: format!("Cannot compute size of union field: {}", field.name),
                        location,
                    }.into());
                }
            }
            // Return as array of I16 (words)
            Ok(IrType::Array { size: max_words, element_type: Box::new(IrType::I16) })
        }
        Type::Function { .. } => {
            // Function types are only valid as part of function pointers
            // When used directly (e.g., in parameter types), treat as a pointer
            Ok(IrType::FatPtr(Box::new(IrType::I16)))
        }
        Type::Enum { .. } => {
            // Enums are treated as integers
            Ok(IrType::I16)
        }
        Type::Typedef(name) => {
            // Typedefs should have been resolved during typed_ast conversion
            // This is an internal error if we reach here
            eprintln!("ERROR: Typedef '{}' reached codegen at {}:{}", name, location.line, location.column);
            eprintln!("Stack trace:");
            eprintln!("{:?}", std::backtrace::Backtrace::capture());
            Err(CodegenError::InvalidType {
                ast_type: ast_type.clone(),
                location,
            }.into())
        }
        Type::Error => Err(CodegenError::InvalidType {
            ast_type: ast_type.clone(),
            location,
        }.into()),
    }
}