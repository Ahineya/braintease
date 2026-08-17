//! Jump statement code generation (break, continue, return)

use super::TypedStatementGenerator;
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
        let mut expr_gen = gen.create_expression_generator();
        let ret_type = ret_expr.get_type();
        
        // Aggregates generate as pointers; the calling convention copies them.
        if ret_type.is_aggregate() {
            let struct_ptr = expr_gen.generate(ret_expr)?;
            gen.builder.build_return(Some(struct_ptr))?;
        } else {
            let ret_val = expr_gen.generate(ret_expr)?;
            gen.builder.build_return(Some(ret_val))?;
        }
    } else {
        gen.builder.build_return(None)?;
    }
    Ok(())
}