//! Global variable code generation

use std::collections::{HashMap, HashSet};
use crate::ir::{Value, IrBuilder, Module, GlobalVariable, Linkage};
use crate::typed_ast::TypedExpr;
use crate::types::{Type, BankTag};
use crate::CompilerError;
use crate::codegen::CodegenError;
use super::super::{VarInfo, expressions::TypedExpressionGenerator, types::complete_type_from_initializer};
use super::utils::convert_type_default;

/// An incomplete extern array reserves no storage, but its element type is
/// enough to form addresses. A later definition supplies its actual extent.
pub(crate) fn declaration_ir_type(ty: &Type, storage_class: crate::StorageClass) -> Result<crate::ir::IrType, CompilerError> {
    if storage_class == crate::StorageClass::Extern {
        if let Type::Array { element_type, size: None } = ty {
            return Ok(crate::ir::IrType::Array {
                element_type: Box::new(convert_type_default(element_type)?), size: 0,
            });
        }
    }
    convert_type_default(ty)
}

/// Generate IR for a global variable
pub fn generate_global_variable(
    builder: &mut IrBuilder,
    module: &mut Module,
    variables: &mut HashMap<String, VarInfo>,
    array_variables: &mut HashSet<String>,
    parameter_variables: &HashSet<String>,
    string_literals: &mut HashMap<String, String>,
    next_string_id: &mut u32,
    name: &str,
    var_type: &Type,
    initializer: Option<&TypedExpr>,
    storage_class: crate::StorageClass,
) -> Result<(), CompilerError> {
    // Handle initializer first to determine array size if needed
    let init_value = if let Some(init_expr) = initializer {
        // We need to generate the initializer value
        // For global variables, only constant initializers are allowed
        let mut expr_gen = TypedExpressionGenerator {
            builder,
            module,
            variables,
            array_variables,
            parameter_variables,
            string_literals,
            next_string_id,
        };
        
        match generate_constant_initializer(&mut expr_gen, init_expr) {
            Ok(value) => match value {
                Value::Constant(_) | Value::ConstantArray(_) | Value::ConstantAggregate(_) | Value::Function(_) => Some(value),
                Value::FatPtr(ref fp) => {
                    // Allow FatPtr for pointer initializers (e.g., string literals)
                    // But only if the address is a constant or global
                    match fp.addr.as_ref() {
                        Value::Global(_) | Value::Constant(_) | Value::Function(_) | Value::AddressOffset { .. } => Some(value),
                        _ => {
                            return Err(CodegenError::UnsupportedConstruct {
                                construct: format!("Non-constant pointer initializer for global variable '{}'", name),
                                location: rcc_common::SourceLocation::new_simple(0, 0),
                            }.into());
                        }
                    }
                }
                Value::Global(_) => Some(value), // Allow references to other globals
                _ => {
                    return Err(CodegenError::UnsupportedConstruct {
                        construct: format!("Non-constant initializer for global variable '{}': {:?}", name, value),
                        location: rcc_common::SourceLocation::new_simple(0, 0),
                    }.into());
                }
            }
            Err(e) => return Err(CodegenError::InternalError {
                message: format!("Initializer for global '{name}': {e}"),
                location: rcc_common::SourceLocation::new_simple(0, 0),
            }.into()),
        }
    } else {
        None
    };
    
    // Use the general helper to complete incomplete types
    let completed_type = complete_type_from_initializer(var_type, initializer);
    let ir_type = declaration_ir_type(&completed_type, storage_class)?;
    
    let linkage = match storage_class {
        crate::StorageClass::Static => Linkage::Internal,
        crate::StorageClass::Extern if initializer.is_none() => Linkage::Declaration,
        _ => Linkage::External,
    };

    // `extern T x;` plus a later tentative definition or definition in the same
    // TU must not emit two globals. Merge into the existing one.
    if let Some(existing) = module.globals.iter_mut().find(|g| g.name == name) {
        if linkage != Linkage::Declaration {
            existing.linkage = linkage;
            existing.var_type = ir_type.clone();
        }
        if let Some(init) = init_value {
            if existing.initializer.is_some() {
                return Err(CodegenError::InternalError {
                    message: format!("Redefinition of global variable '{name}'"),
                    location: rcc_common::SourceLocation::new_simple(0, 0),
                }.into());
            }
            existing.initializer = Some(init);
        }
        if !variables.contains_key(name) {
            variables.insert(name.to_string(), VarInfo {
                value: Value::Global(name.to_string()),
                ir_type,
                bank: Some(BankTag::Global),
            });
        }
        if matches!(var_type, Type::Array { .. }) {
            array_variables.insert(name.to_string());
        }
        return Ok(());
    }
    
    let global = GlobalVariable {
        name: name.to_string(),
        var_type: ir_type.clone(),
        is_constant: false,
        initializer: init_value,
        linkage,
        symbol_id: None,
    };
    
    module.add_global(global);
    
    // Add to variables map for later reference
    variables.insert(name.to_string(), VarInfo {
        value: Value::Global(name.to_string()),
        ir_type,
        bank: Some(BankTag::Global),
    });
    
    // Track global arrays
    if matches!(var_type, Type::Array { .. }) {
        array_variables.insert(name.to_string());
    }
    
    Ok(())
}
/// Preserve aggregate element boundaries until layout is known in the backend.
fn generate_constant_initializer(
    gen: &mut TypedExpressionGenerator,
    expr: &TypedExpr,
) -> Result<Value, CompilerError> {
    if let Some(value) = scalar_initializer_constant(expr) { return Ok(Value::Constant(value)); }
    if let Some(bits) = super::constant_float::initializer_bits(expr) { return Ok(Value::Constant(bits)); }
    if let Some(value) = super::constant_address::generate(gen, expr)? { return Ok(value); }
    match expr {
        TypedExpr::Cast { operand, target_type: Type::Pointer { .. }, .. }
            if matches!(operand.get_type(), Type::Pointer { .. } | Type::Function { .. }) => {
            // Function-pointer casts change the callable type, not its address.
            generate_constant_initializer(gen, operand)
        }
        TypedExpr::ArrayInitializer { elements, .. }
        | TypedExpr::CompoundLiteral { initializer: elements, .. } => {
            if matches!(expr.get_type().unqualified(), Type::Union { .. }) {
                let values = if let Some(member) = elements.first() {
                    let value = generate_constant_initializer(gen, member)?;
                    let ty = convert_type_default(member.get_declared_type())?;
                    super::constant_cells::cells(value, &ty)?
                } else { Vec::new() };
                return Ok(Value::ConstantAggregate(values));
            }
            if let Type::Struct { fields, .. } = expr.get_type() {
                if fields.iter().any(|f| f.bit_width.is_some()) {
                    let layout = crate::semantic::struct_layout::calculate_struct_layout(fields, rcc_common::SourceLocation::new_simple(0, 0))?;
                    let mut values = Vec::new();
                    let mut layouts = layout.fields.iter();
                    let mut unit_offset = None;
                    for (index, declared) in fields.iter().enumerate() {
                        if declared.bit_width == Some(0) { continue; }
                        let field = layouts.next().unwrap();
                        if let Some(width) = field.bit_width {
                            if unit_offset != Some(field.offset) { values.push(Value::Constant(0)); unit_offset = Some(field.offset); }
                            if !field.name.is_empty() {
                                let value = elements.get(index).and_then(scalar_initializer_constant).ok_or_else(||
                                    CompilerError::semantic_error("Bit-field global initializer must be an integer constant".into(), rcc_common::SourceLocation::new_simple(0, 0)))?;
                                let Value::Constant(unit) = values.last_mut().unwrap() else { unreachable!() };
                                let value = if matches!(field.field_type.unqualified(), Type::Bool) { (value != 0) as i64 } else { value };
                                *unit |= (value & crate::codegen::expressions::bit_fields::mask(width)).wrapping_shl(field.bit_offset as u32);
                            }
                        } else {
                            unit_offset = None;
                            values.push(generate_constant_initializer(gen, &elements[index])?);
                        }
                    }
                    return Ok(Value::ConstantAggregate(values));
                }
            }
            let values = elements.iter().map(|elem| generate_constant_initializer(gen, elem))
                .collect::<Result<Vec<_>, _>>()?;
            Ok(Value::ConstantAggregate(values))
        }
        _ => gen.generate(expr),
    }
}

pub(super) fn scalar_initializer_constant(expr: &TypedExpr) -> Option<i64> {
    use crate::ast::{BinaryOp as B, UnaryOp as U};
    let value = match expr {
        TypedExpr::IntLiteral { value, .. } => *value,
        TypedExpr::CharLiteral { value, .. } => *value as i64,
        TypedExpr::Unary { op, operand, .. } => {
            let v = scalar_initializer_constant(operand)?;
            match op { U::Plus => v, U::Minus => v.wrapping_neg(), U::BitNot => !v, U::LogicalNot => (v == 0) as i64, _ => return None }
        }
        TypedExpr::Cast { operand, target_type, .. } if target_type.is_integer() => {
            if operand.get_type().is_floating() {
                super::constant_float::cast_integer(operand, target_type)?
            } else if matches!(operand.get_type(), Type::Pointer { .. }) {
                null_based_member_address(operand)?
            } else {
                scalar_initializer_constant(operand)?
            }
        }
        TypedExpr::SizeofExpr { operand, .. } => operand.get_type().size_in_bytes()? as i64,
        TypedExpr::SizeofType { target_type, .. } => target_type.size_in_bytes()? as i64,
        TypedExpr::Conditional { condition, then_expr, else_expr, .. } => {
            return scalar_initializer_constant(if scalar_initializer_constant(condition)? != 0 { then_expr } else { else_expr });
        }
        TypedExpr::Binary { op, left, right, .. } => {
            let a = scalar_initializer_constant(left)?;
            if *op == B::LogicalAnd && a == 0 { return Some(0); }
            if *op == B::LogicalOr && a != 0 { return Some(1); }
            let b = scalar_initializer_constant(right)?;
            let common = crate::codegen::expressions::usual_arithmetic_type(left.get_type(), right.get_type());
            let unsigned = common.is_unsigned_integer();
            let (a, b) = if matches!(op, B::LeftShift | B::RightShift) { (a, b) } else {
                (normalize_initializer_integer(a, &common), normalize_initializer_integer(b, &common))
            };
            match op {
                B::Add => a.wrapping_add(b), B::Sub => a.wrapping_sub(b), B::Mul => a.wrapping_mul(b),
                B::Div if b != 0 => if unsigned { ((a as u64) / (b as u64)) as i64 } else { a.wrapping_div(b) },
                B::Mod if b != 0 => if unsigned { ((a as u64) % (b as u64)) as i64 } else { a.wrapping_rem(b) },
                B::BitAnd => a & b, B::BitOr => a | b, B::BitXor => a ^ b,
                B::LeftShift if (0..64).contains(&b) => a.wrapping_shl(b as u32),
                B::RightShift if (0..64).contains(&b) => if left.get_type().is_unsigned_integer() { ((a as u64) >> b) as i64 } else { a >> b },
                B::Equal => (a == b) as i64, B::NotEqual => (a != b) as i64,
                B::Less => if unsigned { ((a as u64) < b as u64) as i64 } else { (a < b) as i64 },
                B::Greater => if unsigned { ((a as u64) > b as u64) as i64 } else { (a > b) as i64 },
                B::LessEqual => if unsigned { ((a as u64) <= b as u64) as i64 } else { (a <= b) as i64 },
                B::GreaterEqual => if unsigned { ((a as u64) >= b as u64) as i64 } else { (a >= b) as i64 },
                B::LogicalAnd => (a != 0 && b != 0) as i64, B::LogicalOr => (a != 0 || b != 0) as i64,
                _ => return None,
            }
        }
        _ => return None,
    };
    Some(normalize_initializer_integer(value, expr.get_type()))
}

fn normalize_initializer_integer(value: i64, ty: &Type) -> i64 {
    match ty {
        Type::Bool => (value != 0) as i64,
        Type::Char | Type::SignedChar | Type::Short | Type::Int => value as i16 as i64,
        Type::UnsignedChar | Type::UnsignedShort | Type::UnsignedInt => value as u16 as i64,
        Type::Long => value as i32 as i64,
        Type::UnsignedLong => value as u32 as i64,
        _ => value,
    }
}

// The implementation of offsetof in stddef.h forms a member address from a
// null base. Fold this layout query without emitting a runtime dereference.
fn null_based_member_address(expr: &TypedExpr) -> Option<i64> {
    use crate::ast::UnaryOp;
    fn object(expr: &TypedExpr) -> Option<i64> {
        match expr {
            TypedExpr::MemberAccess { object: base, offset, is_pointer, bit_field: None, .. } => {
                let base = if *is_pointer { null_based_member_address(base)? } else { object(base)? };
                base.checked_add(*offset as i64)
            }
            TypedExpr::Unary { op: UnaryOp::Dereference, operand, .. } => null_based_member_address(operand),
            TypedExpr::ArrayIndex { array, index, elem_type, .. } => {
                object(array)?.checked_add(scalar_initializer_constant(index)?.checked_mul(elem_type.size_in_words()? as i64)?)
            }
            _ => None,
        }
    }
    match expr {
        TypedExpr::Cast { target_type: Type::Pointer { .. }, operand, .. }
            if scalar_initializer_constant(operand) == Some(0) => Some(0),
        TypedExpr::Unary { op: UnaryOp::AddressOf, operand, .. } => object(operand),
        _ => None,
    }
}
