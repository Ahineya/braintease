//! Control flow statement code generation (if, while, for)

use super::TypedStatementGenerator;
use crate::typed_ast::{TypedStmt, TypedExpr};
use crate::ir::{IrBinaryOp, IrType, Value};
use crate::codegen::CodegenError;
use crate::CompilerError;
use rcc_common::LabelId;
use std::collections::HashMap;

pub struct SwitchContext {
    pub cases: HashMap<i64, LabelId>,
    pub default: Option<LabelId>,
}

fn generate_condition(
    gen: &mut TypedStatementGenerator,
    condition: &TypedExpr,
) -> Result<Value, CompilerError> {
    let mut expr_gen = gen.create_expression_generator();
    let cond_val = expr_gen.generate(condition)?;
    crate::codegen::expressions::convert_to_bool(&mut expr_gen, cond_val, condition.get_type())
}

pub fn generate_if(
    gen: &mut TypedStatementGenerator,
    condition: &TypedExpr,
    then_stmt: &TypedStmt,
    else_stmt: Option<&TypedStmt>,
) -> Result<(), CompilerError> {
    let cond_val = generate_condition(gen, condition)?;
    
    let then_label = gen.builder.new_label();
    let else_label = gen.builder.new_label();
    let end_label = gen.builder.new_label();
    
    // Branch on condition
    gen.builder.build_branch_cond(
        cond_val,
        then_label,
        if else_stmt.is_some() { else_label } else { end_label },
    )?;
    
    // Then block
    gen.builder.create_block(then_label)?;
    gen.generate(then_stmt)?;
    if !gen.builder.current_block_has_terminator() {
        gen.builder.build_branch(end_label)?;
    }
    
    // Else block (if present)
    if let Some(else_stmt) = else_stmt {
        gen.builder.create_block(else_label)?;
        gen.generate(else_stmt)?;
        if !gen.builder.current_block_has_terminator() {
            gen.builder.build_branch(end_label)?;
        }
    }
    
    // End label
    gen.builder.create_block(end_label)?;
    
    Ok(())
}

pub fn generate_while(
    gen: &mut TypedStatementGenerator,
    condition: &TypedExpr,
    body: &TypedStmt,
) -> Result<(), CompilerError> {
    let cond_label = gen.builder.new_label();
    let body_label = gen.builder.new_label();
    let end_label = gen.builder.new_label();
    
    // Set up break/continue targets
    gen.break_labels.push(end_label);
    gen.continue_labels.push(cond_label);
    
    // Jump to condition
    gen.builder.build_branch(cond_label)?;
    
    // Condition check - mark as loop condition block
    gen.builder.create_loop_condition_block(cond_label)?;
    let cond_val = generate_condition(gen, condition)?;
    gen.builder.build_branch_cond(cond_val, body_label, end_label)?;
    
    // Body
    gen.builder.create_block(body_label)?;
    gen.generate(body)?;
    if !gen.builder.current_block_has_terminator() {
        gen.builder.build_branch(cond_label)?;
    }
    
    // End
    gen.builder.create_block(end_label)?;
    
    // Clean up break/continue targets
    gen.break_labels.pop();
    gen.continue_labels.pop();
    
    Ok(())
}

pub fn generate_do_while(
    gen: &mut TypedStatementGenerator,
    condition: &TypedExpr,
    body: &TypedStmt,
) -> Result<(), CompilerError> {
    let body_label = gen.builder.new_label();
    let cond_label = gen.builder.new_label();
    let end_label = gen.builder.new_label();

    gen.break_labels.push(end_label);
    gen.continue_labels.push(cond_label);

    gen.builder.build_branch(body_label)?;

    gen.builder.create_block(body_label)?;
    gen.generate(body)?;
    if !gen.builder.current_block_has_terminator() {
        gen.builder.build_branch(cond_label)?;
    }

    gen.builder.create_loop_condition_block(cond_label)?;
    let cond_val = generate_condition(gen, condition)?;
    gen.builder.build_branch_cond(cond_val, body_label, end_label)?;

    gen.builder.create_block(end_label)?;

    gen.break_labels.pop();
    gen.continue_labels.pop();

    Ok(())
}

pub fn generate_for(
    gen: &mut TypedStatementGenerator,
    init: Option<&TypedStmt>,
    condition: Option<&TypedExpr>,
    update: Option<&TypedExpr>,
    body: &TypedStmt,
) -> Result<(), CompilerError> {
    // Initialization
    let outer_variables = gen.variables.clone();
    let outer_arrays = gen.array_variables.clone();
    if let Some(init_stmt) = init {
        gen.generate(init_stmt)?;
    }
    
    let cond_label = gen.builder.new_label();
    let body_label = gen.builder.new_label();
    let update_label = gen.builder.new_label();
    let end_label = gen.builder.new_label();
    
    // Set up break/continue targets
    gen.break_labels.push(end_label);
    gen.continue_labels.push(update_label);
    
    // Jump to condition
    gen.builder.build_branch(cond_label)?;
    
    // Condition check - mark as loop condition block
    gen.builder.create_loop_condition_block(cond_label)?;
    if let Some(cond_expr) = condition {
        let mut expr_gen = gen.create_expression_generator();
        let cond_val = expr_gen.generate(cond_expr)?;
        let cond_val = crate::codegen::expressions::convert_to_bool(&mut expr_gen, cond_val, cond_expr.get_type())?;
        gen.builder.build_branch_cond(cond_val, body_label, end_label)?;
    } else {
        // No condition means always true
        gen.builder.build_branch(body_label)?;
    }
    
    // Body
    gen.builder.create_block(body_label)?;
    gen.generate(body)?;
    if !gen.builder.current_block_has_terminator() {
        gen.builder.build_branch(update_label)?;
    }
    
    // Update
    gen.builder.create_block(update_label)?;
    if let Some(update_expr) = update {
        let mut expr_gen = gen.create_expression_generator();
        expr_gen.generate(update_expr)?;
    }
    gen.builder.build_branch(cond_label)?;
    
    // End
    gen.builder.create_block(end_label)?;
    
    // Clean up break/continue targets
    gen.break_labels.pop();
    gen.continue_labels.pop();

    *gen.variables = outer_variables;
    *gen.array_variables = outer_arrays;
    
    Ok(())
}

fn collect_switch_labels(
    stmt: &TypedStmt,
    cases: &mut HashMap<i64, LabelId>,
    default: &mut Option<LabelId>,
    builder: &mut crate::ir::IrBuilder,
) -> Result<(), CompilerError> {
    match stmt {
        TypedStmt::Switch { .. } => {
            // Nested switch: its labels belong to the inner switch.
            Ok(())
        }
        TypedStmt::Case { value, statement } => {
            if cases.contains_key(value) {
                return Err(CodegenError::UnsupportedConstruct {
                    construct: format!("duplicate case value {value}"),
                    location: rcc_common::SourceLocation::new_simple(0, 0),
                }.into());
            }
            cases.insert(*value, builder.new_label());
            collect_switch_labels(statement, cases, default, builder)
        }
        TypedStmt::Default { statement } => {
            if default.is_some() {
                return Err(CodegenError::UnsupportedConstruct {
                    construct: "multiple default labels in switch".to_string(),
                    location: rcc_common::SourceLocation::new_simple(0, 0),
                }.into());
            }
            *default = Some(builder.new_label());
            collect_switch_labels(statement, cases, default, builder)
        }
        TypedStmt::Compound(stmts) | TypedStmt::DeclarationGroup(stmts) => {
            for s in stmts {
                collect_switch_labels(s, cases, default, builder)?;
            }
            Ok(())
        }
        TypedStmt::If { then_stmt, else_stmt, .. } => {
            collect_switch_labels(then_stmt, cases, default, builder)?;
            if let Some(e) = else_stmt {
                collect_switch_labels(e, cases, default, builder)?;
            }
            Ok(())
        }
        TypedStmt::While { body, .. } | TypedStmt::For { body, .. } | TypedStmt::DoWhile { body, .. } => {
            collect_switch_labels(body, cases, default, builder)
        }
        TypedStmt::Label { statement, .. } => {
            collect_switch_labels(statement, cases, default, builder)
        }
        _ => Ok(()),
    }
}

pub fn generate_switch(
    gen: &mut TypedStatementGenerator,
    expression: &TypedExpr,
    body: &TypedStmt,
) -> Result<(), CompilerError> {
    let mut expr_gen = gen.create_expression_generator();
    let switch_val = expr_gen.generate(expression)?;
    // Integer promotion preserves long/long long controls. Keeping the saved
    // selector in one cell silently discarded their upper words before dispatch.
    let switch_type = expression.get_type().integer_promoted();
    let switch_ir_type = crate::codegen::types::convert_type(
        &switch_type, rcc_common::SourceLocation::new_simple(0, 0),
    )?;
    let switch_val = crate::codegen::expressions::convert_integer(
        &mut expr_gen, switch_val, expression.get_type(), &switch_type,
    )?;
    let switch_ptr = gen.builder.build_alloca(switch_ir_type.clone(), None)?;
    gen.builder.build_store(switch_val, switch_ptr.clone(), switch_ir_type.clone())?;

    let mut cases = HashMap::new();
    let mut default_label = None;
    collect_switch_labels(body, &mut cases, &mut default_label, gen.builder)?;

    let end_label = gen.builder.new_label();
    gen.break_labels.push(end_label);
    gen.switch_contexts.push(SwitchContext {
        cases: cases.clone(),
        default: default_label,
    });

    // Read once with the promoted width. Every dispatch block uses this SSA
    // value, so a large switch does not repeatedly access logical stack RAM.
    let selector = Value::Temp(gen.builder.build_load(switch_ptr, switch_ir_type.clone())?);
    let mut case_list: Vec<(i64, LabelId)> = cases.into_iter().collect();
    let bits = match switch_ir_type { IrType::I64 => 64, IrType::I32 => 32, _ => 16 };
    let unsigned = switch_type.is_unsigned_integer();
    // Case constants are converted to the promoted selector type before
    // ordering. For example, -1 sorts last for an unsigned control, not first.
    let key = |value: i64| {
        let mask = u64::MAX >> (64 - bits);
        ((value as u64) & mask) ^ if unsigned { 0 } else { 1u64 << (bits - 1) }
    };
    case_list.sort_by_key(|(v, _)| key(*v));
    if case_list.windows(2).any(|pair| key(pair[0].0) == key(pair[1].0)) {
        return Err(CodegenError::UnsupportedConstruct {
            construct: "duplicate case value after conversion to switch type".into(),
            location: rcc_common::SourceLocation::new_simple(0, 0),
        }.into());
    }
    generate_switch_dispatch(gen, &selector, &case_list,
                             default_label.unwrap_or(end_label), unsigned)?;

    // Unreachable block for any prefix statements before the first case.
    let prefix = gen.builder.new_label();
    gen.builder.create_block(prefix)?;
    gen.generate(body)?;
    if !gen.builder.current_block_has_terminator() {
        gen.builder.build_branch(end_label)?;
    }

    gen.builder.create_block(end_label)?;
    gen.break_labels.pop();
    gen.switch_contexts.pop();
    Ok(())
}

/// Small leaves keep dispatch compact; larger switches have logarithmic
/// decision depth on every backend, without adding a new serialized IR opcode.
/// Body labels stay in source order, preserving fallthrough, goto and nesting.
fn generate_switch_dispatch(
    gen: &mut TypedStatementGenerator,
    selector: &Value,
    cases: &[(i64, LabelId)],
    fallback: LabelId,
    unsigned: bool,
) -> Result<(), CompilerError> {
    if cases.len() > 3 {
        let middle = cases.len() / 2;
        let left = gen.builder.new_label();
        let right = gen.builder.new_label();
        let less = gen.builder.build_binary(
            if unsigned { IrBinaryOp::Ult } else { IrBinaryOp::Slt },
            selector.clone(), Value::Constant(cases[middle].0), IrType::I16,
        )?;
        gen.builder.build_branch_cond(Value::Temp(less), left, right)?;
        gen.builder.create_block(left)?;
        generate_switch_dispatch(gen, selector, &cases[..middle], fallback, unsigned)?;
        gen.builder.create_block(right)?;
        return generate_switch_dispatch(gen, selector, &cases[middle..], fallback, unsigned);
    }
    for &(value, label) in cases {
        let eq = gen.builder.build_binary(
            IrBinaryOp::Eq,
            selector.clone(),
            Value::Constant(value),
            IrType::I16,
        )?;
        let next = gen.builder.new_label();
        gen.builder.build_branch_cond(Value::Temp(eq), label, next)?;
        gen.builder.create_block(next)?;
    }
    gen.builder.build_branch(fallback)?;
    Ok(())
}

pub fn generate_case(
    gen: &mut TypedStatementGenerator,
    value: i64,
    statement: &TypedStmt,
) -> Result<(), CompilerError> {
    let label = gen.switch_contexts.last()
        .and_then(|ctx| ctx.cases.get(&value).copied())
        .ok_or_else(|| CodegenError::UnsupportedConstruct {
            construct: "case label outside of switch".to_string(),
            location: rcc_common::SourceLocation::new_simple(0, 0),
        })?;
    if !gen.builder.current_block_has_terminator() {
        gen.builder.build_branch(label)?;
    }
    gen.builder.create_block(label)?;
    gen.generate(statement)
}

pub fn generate_default(
    gen: &mut TypedStatementGenerator,
    statement: &TypedStmt,
) -> Result<(), CompilerError> {
    let label = gen.switch_contexts.last()
        .and_then(|ctx| ctx.default)
        .ok_or_else(|| CodegenError::UnsupportedConstruct {
            construct: "default label outside of switch".to_string(),
            location: rcc_common::SourceLocation::new_simple(0, 0),
        })?;
    if !gen.builder.current_block_has_terminator() {
        gen.builder.build_branch(label)?;
    }
    gen.builder.create_block(label)?;
    gen.generate(statement)
}
