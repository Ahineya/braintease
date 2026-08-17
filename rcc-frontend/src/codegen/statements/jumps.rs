//! Jump statement code generation (break, continue, return, goto, labels)

use super::TypedStatementGenerator;
use super::super::expressions::convert_integer;
use crate::typed_ast::TypedExpr;
use crate::codegen::CodegenError;
use crate::CompilerError;

pub fn generate_break(gen: &mut TypedStatementGenerator) -> Result<(), CompilerError> {
    if let Some(label) = gen.break_labels.last() {
        gen.builder.build_branch(*label)?;
        Ok(())
    } else {
        Err(CodegenError::InvalidBreak {
            location: rcc_common::SourceLocation::new_simple(0, 0),
        }.into())
    }
}

pub fn generate_continue(gen: &mut TypedStatementGenerator) -> Result<(), CompilerError> {
    if let Some(label) = gen.continue_labels.last() {
        gen.builder.build_branch(*label)?;
        Ok(())
    } else {
        Err(CodegenError::InvalidContinue {
            location: rcc_common::SourceLocation::new_simple(0, 0),
        }.into())
    }
}

pub fn generate_return(
    gen: &mut TypedStatementGenerator,
    expr: Option<&TypedExpr>,
) -> Result<(), CompilerError> {
    if let Some(ret_expr) = expr {
        let ret_type = ret_expr.get_type();
        let fn_ret = gen.builder.current_return_type().cloned();
        let mut expr_gen = gen.create_expression_generator();
        
        // Aggregates generate as pointers; the calling convention copies them.
        if ret_type.is_aggregate() {
            let struct_ptr = expr_gen.generate(ret_expr)?;
            gen.builder.build_return(Some(struct_ptr))?;
        } else {
            let ret_val = expr_gen.generate(ret_expr)?;
            let dest_ty = match fn_ret {
                Some(crate::ir::IrType::I64) => Some(crate::types::Type::LongLong),
                Some(crate::ir::IrType::I32) => Some(crate::types::Type::Long),
                _ => None,
            };
            let ret_val = if ret_type.is_integer() {
                if let Some(dest_ty) = dest_ty {
                    if dest_ty.size_in_words() != ret_type.size_in_words() {
                        convert_integer(&mut expr_gen, ret_val, ret_type, &dest_ty)?
                    } else {
                        ret_val
                    }
                } else {
                    ret_val
                }
            } else {
                ret_val
            };
            gen.builder.build_return(Some(ret_val))?;
        }
    } else {
        gen.builder.build_return(None)?;
    }
    Ok(())
}

pub fn generate_goto(
    gen: &mut TypedStatementGenerator,
    name: &str,
) -> Result<(), CompilerError> {
    let label = gen.get_or_create_goto_label(name);
    if !gen.builder.current_block_has_terminator() {
        gen.builder.build_branch(label)?;
    }
    Ok(())
}

pub fn generate_label(
    gen: &mut TypedStatementGenerator,
    name: &str,
    statement: &crate::typed_ast::TypedStmt,
) -> Result<(), CompilerError> {
    if !gen.defined_goto_labels.insert(name.to_string()) {
        return Err(CodegenError::DuplicateLabel {
            name: name.to_string(),
            location: rcc_common::SourceLocation::new_simple(0, 0),
        }
        .into());
    }
    let label = gen.get_or_create_goto_label(name);
    if !gen.builder.current_block_has_terminator() {
        gen.builder.build_branch(label)?;
    }
    gen.builder.create_block(label)?;
    gen.generate(statement)
}