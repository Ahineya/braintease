//! Store aggregate initializers (arrays, structs, unions, compound literals)

use super::TypedExpressionGenerator;
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
    match (dest_type.unqualified(), init) {
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
            for field in &layout.fields {
                if field.bit_width.is_some() {
                    let ptr = field_gep(gen, dest.clone(), dest_type, &field.field_type, field.offset as i64)?;
                    gen.builder.build_store_volatile(Value::Constant(0), ptr, convert_type_default(&field.field_type)?, dest_type.is_volatile())?;
                }
            }
            let mut layouts = layout.fields.iter();
            for (i, declared) in fields.iter().enumerate() {
                if declared.bit_width == Some(0) { continue; }
                let field = layouts.next().unwrap();
                let qualified_field = dest_type.qualify_member(&field.field_type);
                let ptr = field_gep(gen, dest.clone(), dest_type, &field.field_type, field.offset as i64)?;
                let zero = zero_typed(&field.field_type);
                let init = elements.get(i).unwrap_or(&zero);
                if let Some(width) = field.bit_width {
                    if !field.name.is_empty() {
                        let raw = gen.generate(init)?;
                        let val = if matches!(field.field_type.unqualified(), Type::Bool) {
                            super::conversions::convert_to_bool(gen, raw, init.get_type())?
                        } else { super::conversions::convert_integer(gen, raw, init.get_type(), &field.field_type)? };
                        super::bit_fields::store_unit(gen, ptr, &qualified_field, width, field.bit_offset, val, None)?;
                    }
                } else { store_initializer(gen, ptr, &qualified_field, init)?; }
            }
            Ok(())
        }
        (Type::Union { .. }, TypedExpr::ArrayInitializer { elements, .. })
        | (Type::Union { .. }, TypedExpr::CompoundLiteral { initializer: elements, .. }) => {
            if let Some(first) = elements.first() {
                store_initializer(gen, dest, &dest_type.qualify_member(first.get_declared_type()), first)?;
            }
            Ok(())
        }
        (ty, _) if ty.is_aggregate() => {
            let src = gen.generate(init)?;
            super::assignments::copy_struct_access(gen, src, dest, init.get_declared_type(), dest_type)
        }
        (Type::Pointer { .. }, TypedExpr::IntLiteral { value: 0, .. })
        | (Type::Pointer { .. }, TypedExpr::CharLiteral { value: 0, .. }) => {
            gen.builder
                .build_store_volatile(
                    Value::FatPtr(FatPointer {
                        addr: Box::new(Value::Constant(0)),
                        bank: BankTag::Null,
                    }),
                    dest,
                    convert_type_default(dest_type)?,
                    dest_type.is_volatile(),
                )
                .map_err(|e| intern_err(e))
        }
        (Type::Pointer { .. }, TypedExpr::CompoundLiteral { .. }) => {
            let val = gen.generate(init)?;
            gen.builder.build_store_volatile(val, dest, convert_type_default(dest_type)?, dest_type.is_volatile()).map_err(|e| intern_err(e))
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
            let stored = if matches!(dest_type.unqualified(), Type::Bool) {
                super::conversions::convert_to_bool(gen, val, init.get_type())
                    .map_err(|e| intern_err(format!("{e}")))?
            } else if dest_type.is_floating() || init.get_type().is_floating() {
                super::conversions::convert_value(gen, val, init.get_type(), dest_type)
                    .map_err(|e| intern_err(format!("{e}")))?
            } else if dest_type.is_integer() && init.get_type().is_integer() {
                super::conversions::convert_integer(gen, val, init.get_type(), dest_type)
                    .map_err(|e| intern_err(format!("{e}")))?
            } else {
                val
            };
            gen.builder.build_store_volatile(stored, dest, convert_type_default(dest_type)?, dest_type.is_volatile()).map_err(|e| intern_err(e))
        }
    }
}

fn zero_typed(ty: &Type) -> TypedExpr {
    match ty.unqualified() {
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
        Type::Float | Type::Double => TypedExpr::FloatLiteral {
            bits: 0,
            expr_type: ty.clone(),
        },
        _ => TypedExpr::IntLiteral {
            value: 0,
            expr_type: Type::Int,
        },
    }
}
