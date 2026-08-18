//! Statement semantic analysis
//! 
//! This module handles semantic analysis of statements and declarations.

use crate::StorageClass;
use crate::ast::*;
use crate::semantic::errors::SemanticError;
use crate::semantic::expressions::ExpressionAnalyzer;
use rcc_common::{CompilerError, SourceLocation};
use std::cell::RefCell;
use std::rc::Rc;
use crate::semantic::expressions::initializers::InitializerAnalyzer;
use crate::Type;

pub struct StatementAnalyzer {
    pub current_function: Option<Type>,
    pub expression_analyzer: Rc<RefCell<ExpressionAnalyzer>>,
    pub initializer_analyzer: Rc<RefCell<InitializerAnalyzer>>,
    switch_depth: RefCell<u32>,
}

impl StatementAnalyzer {
    /// Create a new statement analyzer
    pub fn new(
        current_function: Option<Type>,
        expression_analyzer: Rc<RefCell<ExpressionAnalyzer>>,
        initializer_analyzer: Rc<RefCell<InitializerAnalyzer>>,
    ) -> Self {
        Self {
            current_function,
            expression_analyzer,
            initializer_analyzer,
            switch_depth: RefCell::new(0),
        }
    }
    
    /// Analyze a statement
    pub fn analyze_statement(
        &self,
        stmt: &mut Statement,
    ) -> Result<(), CompilerError> {
        match &mut stmt.kind {
            StatementKind::Expression(expr) => {
                self.expression_analyzer.borrow().analyze(expr)?;
            }
            
            StatementKind::Compound(statements) => {
                // Enter new scope for compound statement
                self.expression_analyzer.borrow().type_analyzer.borrow().symbol_table.borrow_mut().push_scope();
                
                for stmt in statements {
                    self.analyze_statement(stmt)?;
                }

                self.expression_analyzer.borrow().type_analyzer.borrow().symbol_table.borrow_mut().pop_scope();
            }
            
            StatementKind::Declaration { declarations } => {
                for decl in declarations {
                    self.analyze_declaration(decl)?;
                }
            }
            
            StatementKind::If { condition, then_stmt, else_stmt } => {
                let analyzer = self.expression_analyzer.borrow();
                analyzer.analyze(condition)?;
                analyzer.check_boolean_context(condition)?;
                
                self.analyze_statement(then_stmt)?;
                
                if let Some(else_stmt) = else_stmt {
                    self.analyze_statement(else_stmt)?;
                }
            }
            
            StatementKind::While { condition, body } => {
                let analyzer = self.expression_analyzer.borrow();
                analyzer.analyze(condition)?;
                analyzer.check_boolean_context(condition)?;
                
                self.analyze_statement(body)?;
            }
            
            StatementKind::For { init, condition, update, body } => {
                // Enter new scope for for-loop
                self.expression_analyzer.borrow().type_analyzer.borrow().symbol_table.borrow_mut().push_scope();
                
                if let Some(init) = init {
                    self.analyze_statement(init)?;
                }
                
                if let Some(condition) = condition {
                    let analyzer = self.expression_analyzer.borrow();
                    analyzer.analyze(condition)?;
                    analyzer.check_boolean_context(condition)?;
                }
                
                if let Some(update) = update {
                    let analyzer = self.expression_analyzer.borrow();
                    analyzer.analyze(update)?;
                }
                
                self.analyze_statement(body)?;

                self.expression_analyzer.borrow().type_analyzer.borrow().symbol_table.borrow_mut().pop_scope();
            }
            
            StatementKind::DoWhile { body, condition } => {
                self.analyze_statement(body)?;

                let analyzer = self.expression_analyzer.borrow();
                analyzer.analyze(condition)?;
                analyzer.check_boolean_context(condition)?;
            }

            StatementKind::Switch { expression, body } => {
                let analyzer = self.expression_analyzer.borrow();
                analyzer.analyze(expression)?;
                drop(analyzer);
                *self.switch_depth.borrow_mut() += 1;
                let result = self.analyze_statement(body);
                *self.switch_depth.borrow_mut() -= 1;
                result?;
            }

            StatementKind::Case { value, statement } => {
                if *self.switch_depth.borrow() == 0 {
                    return Err(SemanticError::CaseOutsideSwitch {
                        location: stmt.span.start.clone(),
                    }.into());
                }
                self.expression_analyzer.borrow().analyze(value)?;
                self.analyze_statement(statement)?;
            }

            StatementKind::Default { statement } => {
                if *self.switch_depth.borrow() == 0 {
                    return Err(SemanticError::DefaultOutsideSwitch {
                        location: stmt.span.start.clone(),
                    }.into());
                }
                self.analyze_statement(statement)?;
            }
            
            StatementKind::Return(expr_opt) => {
                if let Some(expr) = expr_opt {
                    let analyzer = self.expression_analyzer.borrow();
                    analyzer.analyze(expr)?;
                    
                    // Check return type compatibility with typedef awareness
                    if let Some(expected_return_type) = &self.current_function {
                        if let Some(expr_type) = &expr.expr_type {
                            // Use typedef-aware type compatibility checking
                            if !analyzer.type_analyzer.borrow().is_assignable(expected_return_type, expr_type) {
                                return Err(SemanticError::ReturnTypeMismatch {
                                    expected: expected_return_type.clone(),
                                    found: expr_type.clone(),
                                    location: expr.span.start.clone(),
                                }.into());
                            }
                        }
                    }
                } else {
                    // Return with no value - check if function returns void
                    if let Some(expected_return_type) = &self.current_function {
                        if !matches!(expected_return_type, Type::Void) {
                            return Err(SemanticError::ReturnTypeMismatch {
                                expected: expected_return_type.clone(),
                                found: Type::Void,
                                location: stmt.span.start.clone(),
                            }.into());
                        }
                    }
                }
            }
            
            StatementKind::Break | StatementKind::Continue | StatementKind::Empty => {
                // No semantic analysis needed
            }

            StatementKind::Goto(_) => {
                // Label existence is checked during codegen (function scope).
            }

            StatementKind::Label { statement, .. } => {
                self.analyze_statement(statement)?;
            }
            
            StatementKind::InlineAsm { assembly: _, outputs, inputs, clobbers: _ } => {
                // Analyze inline assembly operands
                
                // For output operands, we need special handling
                // They are lvalues (write destinations), not values to read
                for op in outputs {
                    // For output operands, we just need to ensure they're valid lvalues
                    // and get their types, but we don't evaluate them as expressions
                    match &mut op.expr.kind {
                        ExpressionKind::Identifier { name, symbol_id } => {
                            // Look up the symbol to verify it exists and get its type
                            if let Some(id) = self.expression_analyzer.borrow().type_analyzer.borrow().symbol_table.borrow().lookup(name) {
                                *symbol_id = Some(id);
                                if let Some(var_type) = self.expression_analyzer.borrow().type_analyzer.borrow().symbol_types.borrow().get(&id) {
                                    op.expr.expr_type = Some(var_type.clone());
                                }
                            } else {
                                // Variable doesn't exist
                                return Err(SemanticError::UndefinedVariable {
                                    name: name.clone(),
                                    location: op.expr.span.start.clone(),
                                }.into());
                            }
                        }
                        _ => {
                            // For other lvalue expressions (array elements, struct fields, etc.)
                            // we still need to analyze them normally
                            
                            self.expression_analyzer.borrow().analyze(&mut op.expr)?;
                        }
                    }
                }
                
                // Input operands are regular expressions that need to be evaluated
                for op in inputs {
                    self.expression_analyzer.borrow().analyze(&mut op.expr)?;
                }
            }
        }
        
        Ok(())
    }
    
    /// Analyze a declaration
    /// Note: This is for local declarations only. Global declarations and typedefs
    /// are handled in the first pass by SymbolManager.
    pub fn analyze_declaration(
        &self,
        decl: &mut Declaration,
    ) -> Result<(), CompilerError> {
        // Handle typedef specially - it defines a type alias, not a variable
        if decl.storage_class == StorageClass::Typedef {
            self.expression_analyzer.borrow().type_analyzer.borrow().register_complete_type_from_decl(&decl.decl_type);
            // Resolve the type first
            
            decl.decl_type = self.expression_analyzer.borrow().type_analyzer.borrow().resolve_type(&decl.decl_type);
            // Register this as a type definition
            self.expression_analyzer.borrow().type_analyzer.borrow().type_definitions.borrow_mut().insert(decl.name.clone(), decl.decl_type.clone());
            return Ok(());
        }

        // Tag-only declaration: `struct T { int x; };` / `struct T;`
        if decl.name.is_empty() {
            match &decl.decl_type {
                Type::Struct { name: Some(name), .. }
                | Type::Union { name: Some(name), .. }
                | Type::Enum { name: Some(name), .. } => {
                    self.expression_analyzer
                        .borrow()
                        .type_analyzer
                        .borrow_mut()
                        .register_type_definition(name.clone(), decl.decl_type.clone())?;
                }
                _ => {}
            }
            return Ok(());
        }
        
        // Check for redefinition in current scope
        if self.expression_analyzer.borrow().type_analyzer.borrow().symbol_table.borrow_mut().exists_in_current_scope(&decl.name) {
            return Err(SemanticError::RedefinedSymbol {
                name: decl.name.clone(),
                original_location: SourceLocation::new_simple(0, 0), // TODO: Track original location
                redefinition_location: decl.span.start.clone(),
            }.into());
        }
        
        // `struct S { int x; } s;` defines tag S as well as the object.
        self.expression_analyzer.borrow().type_analyzer.borrow().register_complete_type_from_decl(&decl.decl_type);

        // Don't resolve typedef types here - preserve them for proper type checking
        // Only resolve struct/union/enum references that need to be looked up
        decl.decl_type = self.expression_analyzer.borrow().type_analyzer.borrow().resolve_struct_references(&decl.decl_type);
        
        // Add to symbol table
        let symbol_id = self.expression_analyzer.borrow().type_analyzer.borrow().symbol_table.borrow_mut().add_symbol(decl.name.clone());
        decl.symbol_id = Some(symbol_id);
        self.expression_analyzer.borrow().type_analyzer.borrow().symbol_locations.borrow_mut().insert(symbol_id, decl.span.start.clone());
        // Store the variable type
        self.expression_analyzer.borrow().type_analyzer.borrow().symbol_types.borrow_mut().insert(symbol_id, decl.decl_type.clone());
        
        // Analyze initializer if present
        if let Some(initializer) = &mut decl.initializer {
            self.initializer_analyzer.borrow().analyze(initializer, &decl.decl_type)?;
        }
        
        Ok(())
    }
}