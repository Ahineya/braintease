//! Statement code generation modules

mod declarations;
mod control_flow;
mod jumps;
mod misc;

pub use declarations::generate_declaration;
pub use control_flow::{generate_if, generate_while, generate_for, generate_do_while, generate_switch, generate_case, generate_default, SwitchContext};
pub use jumps::{generate_break, generate_continue, generate_return, generate_goto, generate_label};
pub use misc::{generate_expression_stmt, generate_compound, generate_inline_asm};

use std::collections::{HashMap, HashSet};
use crate::ir::{IrBuilder, Module};
use crate::typed_ast::TypedStmt;
use crate::CompilerError;
use rcc_common::LabelId as Label;
use super::VarInfo;
use super::expressions::TypedExpressionGenerator;
use crate::codegen::CodegenError;

/// Typed statement generator context
pub struct TypedStatementGenerator<'a> {
    pub builder: &'a mut IrBuilder,
    pub module: &'a mut Module,
    pub variables: &'a mut HashMap<String, VarInfo>,
    pub array_variables: &'a mut HashSet<String>,
    pub parameter_variables: &'a HashSet<String>,
    pub string_literals: &'a mut HashMap<String, String>,
    pub next_string_id: &'a mut u32,
    pub break_labels: &'a mut Vec<Label>,
    pub continue_labels: &'a mut Vec<Label>,
    pub switch_contexts: &'a mut Vec<SwitchContext>,
    /// Function-scoped user labels for `goto` (`name -> IR label`).
    pub goto_labels: HashMap<String, Label>,
    /// Labels that have a definition (as opposed to only a `goto` reference).
    pub defined_goto_labels: HashSet<String>,
}

impl<'a> TypedStatementGenerator<'a> {
    /// Generate IR for a typed statement
    pub fn generate(&mut self, stmt: &TypedStmt) -> Result<(), CompilerError> {
        match stmt {
            TypedStmt::Expression(expr) => {
                misc::generate_expression_stmt(self, expr)
            }
            
            TypedStmt::Declaration { name, decl_type, initializer, symbol_id: _ } => {
                declarations::generate_declaration(self, name, decl_type, initializer.as_ref())
            }
            
            TypedStmt::Compound(statements) => {
                misc::generate_compound(self, statements)
            }
            
            TypedStmt::If { condition, then_stmt, else_stmt } => {
                control_flow::generate_if(self, condition, then_stmt, else_stmt.as_deref())
            }
            
            TypedStmt::While { condition, body } => {
                control_flow::generate_while(self, condition, body)
            }

            TypedStmt::DoWhile { condition, body } => {
                control_flow::generate_do_while(self, condition, body)
            }
            
            TypedStmt::For { init, condition, update, body } => {
                control_flow::generate_for(
                    self,
                    init.as_deref(),
                    condition.as_ref(),
                    update.as_ref(),
                    body,
                )
            }

            TypedStmt::Switch { expression, body } => {
                control_flow::generate_switch(self, expression, body)
            }

            TypedStmt::Case { value, statement } => {
                control_flow::generate_case(self, *value, statement)
            }

            TypedStmt::Default { statement } => {
                control_flow::generate_default(self, statement)
            }
            
            TypedStmt::Return(expr) => {
                jumps::generate_return(self, expr.as_ref())
            }
            
            TypedStmt::Break => {
                jumps::generate_break(self)
            }
            
            TypedStmt::Continue => {
                jumps::generate_continue(self)
            }

            TypedStmt::Goto { name } => {
                jumps::generate_goto(self, name)
            }

            TypedStmt::Label { name, statement } => {
                jumps::generate_label(self, name, statement)
            }
            
            TypedStmt::InlineAsm { assembly, outputs, inputs, clobbers } => {
                // Generate extended inline assembly with operands and clobbers
                misc::generate_inline_asm_extended(self, assembly, outputs, inputs, clobbers)
            }
            
            TypedStmt::Empty => Ok(()),
        }
    }
    
    pub(super) fn create_expression_generator(&mut self) -> TypedExpressionGenerator<'_> {
        TypedExpressionGenerator {
            builder: self.builder,
            module: self.module,
            variables: self.variables,
            array_variables: self.array_variables,
            parameter_variables: self.parameter_variables,
            string_literals: self.string_literals,
            next_string_id: self.next_string_id,
        }
    }

    /// Error if any `goto` targeted a label that was never defined in this function.
    pub fn check_undefined_labels(&self) -> Result<(), CompilerError> {
        for name in self.goto_labels.keys() {
            if !self.defined_goto_labels.contains(name) {
                return Err(CodegenError::UndefinedLabel {
                    name: name.clone(),
                    location: rcc_common::SourceLocation::new_simple(0, 0),
                }
                .into());
            }
        }
        Ok(())
    }

    pub(crate) fn get_or_create_goto_label(&mut self, name: &str) -> Label {
        if let Some(&id) = self.goto_labels.get(name) {
            id
        } else {
            let id = self.builder.new_label();
            self.goto_labels.insert(name.to_string(), id);
            id
        }
    }
}