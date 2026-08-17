//! Store aggregate initializers (arrays, structs, unions, compound literals)

use super::TypedExpressionGenerator;
use super::assignments::copy_struct;
use super::convert_type_default;
use crate::ir::{IrType, Value, FatPointer};
use crate::typed_ast::TypedExpr;
use crate::types::{Type, BankTag};
use crate::codegen::CodegenError;
use crate::CompilerError;

fn loc() -> rcc_common::SourceLocation {
    rcc_common::SourceLocation::new_simple(0, 0)
}

fn intern_err(e: impl std::fmt::Display) -> CompilerError {
    CodegenError::InternalError {
        message: e.to_string(),
        location: loc(),
    }
    .into()
}

fn element_gep(
    gen: &mut TypedExpressionGenerator,
    dest: Value,
    index: i64,
    element_type: &Type,
) -> Result<Value, CompilerError> {
    let elem_ir = convert_type_default(element_type)?;
    let ptr_ty = IrType::FatPtr(Box::new(elem_ir));
    gen.builder
        .build_pointer_offset(dest, Value::Constant(index), ptr_ty)
        .map_err(|e| intern_err(e))
}

fn field_gep(
    gen: &mut TypedExpressionGenerator,
    dest: Value,
    aggregate_type: &Type,
    field_type: &Type,
    offset_words: i64,
) -> Result<Value, CompilerError> {
    let agg_ir = convert_type_default(aggregate_type)?;
    let field_ir = convert_type_default(field_type)?;
    gen.builder
        .build_struct_field_gep(dest, offset_words, agg_ir, field_ir)
        .map_err(|e| intern_err(e))
}

/// Recursively store an initializer into `dest` (a fat pointer to `dest_type`).
pub fn store_initializer(
    gen: &mut TypedExpressionGenerator,
    dest: Value,
    dest_type: &Type,
    init: &TypedExpr,
) -> Result<(), CompilerError> {
    match (dest_type, init) {
        (Type::Array { element_type, .. }, TypedExpr::ArrayInitializer { elements, .. })
        | (Type::Array { element_type, .. }, TypedExpr::CompoundLiteral { initializer: elements, .. }) => {
            for (i, elem) in elements.iter().enumerate() {
                let elem_addr = element_gep(gen, dest.clone(), i as i64, element_type)?;
                store_initializer(gen, elem_addr, element_type, elem)?;
            }
            Ok(())
        }
        (Type::Struct { fields, .. }, TypedExpr::ArrayInitializer { elements, .. })
        | (Type::Struct { fields, .. }, TypedExpr::CompoundLiteral { initializer: elements, .. }) => {
            let layout = crate::semantic::struct_layout::calculate_struct_layout(fields, loc())
                .map_err(|e| intern_err(format!("Failed to calculate struct layout: {e}")))?;
            for (i, field) in layout.fields.iter().enumerate() {
                let field_ptr = field_gep(gen, dest.clone(), dest_type, &field.field_type, field.offset as i64)?;
                if i < elements.len() {
                    store_initializer(gen, field_ptr, &field.field_type, &elements[i])?;
                } else {
                    store_initializer(gen, field_ptr, &field.field_type, &zero_typed(&field.field_type))?;
                }
            }
            Ok(())
        }
        (Type::Union { .. }, TypedExpr::ArrayInitializer { elements, .. })
        | (Type::Union { .. }, TypedExpr::CompoundLiteral { initializer: elements, .. }) => {
            if let Some(first) = elements.first() {
                store_initializer(gen, dest, first.get_type(), first)?;
            }
            Ok(())
        }
        (Type::Struct { .. }, _) => {
            let src = gen.generate(init)?;
            copy_struct(gen, src, dest, dest_type)
        }
        (Type::Pointer { .. }, TypedExpr::IntLiteral { value: 0, .. })
        | (Type::Pointer { .. }, TypedExpr::CharLiteral { value: 0, .. }) => {
            gen.builder
                .build_store(
                    Value::FatPtr(FatPointer {
                        addr: Box::new(Value::Constant(0)),
                        bank: BankTag::Null,
                    }),
                    dest,
                )
                .map_err(|e| intern_err(e))
        }
        (Type::Pointer { .. }, TypedExpr::CompoundLiteral { .. }) => {
            let val = gen.generate(init)?;
            gen.builder.build_store(val, dest).map_err(|e| intern_err(e))
        }
        (_, TypedExpr::ArrayInitializer { elements, .. })
        | (_, TypedExpr::CompoundLiteral { initializer: elements, .. }) => {
            if let Some(first) = elements.first() {
                store_initializer(gen, dest, dest_type, first)
            } else {
                Ok(())
            }
        }
        _ => {
            let val = gen.generate(init)?;
            gen.builder.build_store(val, dest).map_err(|e| intern_err(e))
        }
    }
}

fn zero_typed(ty: &Type) -> TypedExpr {
    match ty {
        Type::Array { element_type, size: Some(n) } => TypedExpr::ArrayInitializer {
            elements: (0..*n).map(|_| zero_typed(element_type)).collect(),
            expr_type: ty.clone(),
        },
        Type::Struct { fields, .. } => TypedExpr::ArrayInitializer {
            elements: fields.iter().map(|f| zero_typed(&f.field_type)).collect(),
            expr_type: ty.clone(),
        },
        Type::Char | Type::SignedChar | Type::UnsignedChar => TypedExpr::CharLiteral {
            value: 0,
            expr_type: ty.clone(),
        },
        _ => TypedExpr::IntLiteral {
            value: 0,
            expr_type: Type::Int,
        },
    }
}
