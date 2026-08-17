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

pub fn generate_if(
    gen: &mut TypedStatementGenerator,
    condition: &TypedExpr,
    then_stmt: &TypedStmt,
    else_stmt: Option<&TypedStmt>,
) -> Result<(), CompilerError> {
    let mut expr_gen = gen.create_expression_generator();
    let cond_val = expr_gen.generate(condition)?;
    
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
    gen.builder.build_branch(end_label)?;
    
    // Else block (if present)
    if let Some(else_stmt) = else_stmt {
        gen.builder.create_block(else_label)?;
        gen.generate(else_stmt)?;
        gen.builder.build_branch(end_label)?;
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
    let mut expr_gen = gen.create_expression_generator();
    let cond_val = expr_gen.generate(condition)?;
    gen.builder.build_branch_cond(cond_val, body_label, end_label)?;
    
    // Body
    gen.builder.create_block(body_label)?;
    gen.generate(body)?;
    gen.builder.build_branch(cond_label)?;
    
    // End
    gen.builder.create_block(end_label)?;
    
    // Clean up break/continue targets
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
        gen.builder.build_branch_cond(cond_val, body_label, end_label)?;
    } else {
        // No condition means always true
        gen.builder.build_branch(body_label)?;
    }
    
    // Body
    gen.builder.create_block(body_label)?;
    gen.generate(body)?;
    gen.builder.build_branch(update_label)?;
    
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
        TypedStmt::Compound(stmts) => {
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
        TypedStmt::While { body, .. } | TypedStmt::For { body, .. } => {
            collect_switch_labels(body, cases, default, builder)
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
    let switch_ptr = gen.builder.build_alloca(IrType::I16, None)?;
    gen.builder.build_store(switch_val, switch_ptr.clone())?;

    let mut cases = HashMap::new();
    let mut default_label = None;
    collect_switch_labels(body, &mut cases, &mut default_label, gen.builder)?;

    let end_label = gen.builder.new_label();
    gen.break_labels.push(end_label);
    gen.switch_contexts.push(SwitchContext {
        cases: cases.clone(),
        default: default_label,
    });

    // Dispatch: compare against each case, then default or end.
    let mut case_list: Vec<(i64, LabelId)> = cases.into_iter().collect();
    case_list.sort_by_key(|(v, _)| *v);
    for (value, label) in case_list {
        let loaded = gen.builder.build_load(switch_ptr.clone(), IrType::I16)?;
        let eq = gen.builder.build_binary(
            IrBinaryOp::Eq,
            Value::Temp(loaded),
            Value::Constant(value),
            IrType::I16,
        )?;
        let next = gen.builder.new_label();
        gen.builder.build_branch_cond(Value::Temp(eq), label, next)?;
        gen.builder.create_block(next)?;
    }
    if let Some(d) = default_label {
        gen.builder.build_branch(d)?;
    } else {
        gen.builder.build_branch(end_label)?;
    }

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