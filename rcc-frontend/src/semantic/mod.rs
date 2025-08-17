//! Semantic Analysis for C99
//! 
//! Performs type checking, symbol resolution, and semantic validation
//! on the AST produced by the parser.

pub mod errors;
pub mod types;
pub mod expressions;
pub mod statements;
pub mod struct_layout;

use crate::ast::*;
use rcc_common::{CompilerError, SymbolTable, SymbolId, SourceLocation, SourceSpan};
use std::collections::HashMap;
use std::cell::{Ref, RefCell};
use std::rc::Rc;
pub use errors::SemanticError;
use crate::semantic::expressions::ExpressionAnalyzer;
use crate::semantic::statements::StatementAnalyzer;
use crate::semantic::expressions::initializers::InitializerAnalyzer;
use crate::semantic::types::TypeAnalyzer;
use crate::Type;

/// Semantic analyzer context
pub struct SemanticAnalyzer {
    symbol_table: Rc<RefCell<SymbolTable>>,
    current_function: Option<Type>,
    symbol_locations: Rc<RefCell<HashMap<SymbolId, SourceLocation>>>,
    symbol_types: Rc<RefCell<HashMap<SymbolId, Type>>>,
    type_definitions: Rc<RefCell<HashMap<String, Type>>>,
    
    type_analyzer: Rc<RefCell<TypeAnalyzer>>,
    
    statement_analyzer: Rc<RefCell<StatementAnalyzer>>,
    expression_analyzer: Rc<RefCell<ExpressionAnalyzer>>,
    initializer_analyzer: Rc<RefCell<InitializerAnalyzer>>,
}

impl Default for SemanticAnalyzer {
    fn default() -> Self {
        Self::new()
    }
}

impl SemanticAnalyzer {
    /// Create a new semantic analyzer
    pub fn new() -> Self {

        let symbol_table = Rc::new(RefCell::new(SymbolTable::new()));
        let symbol_locations = Rc::new(RefCell::new(HashMap::new()));
        let symbol_types = Rc::new(RefCell::new(HashMap::new()));
        let type_definitions = Rc::new(RefCell::new(HashMap::new()));
        
        let type_analyzer = Rc::new(RefCell::new(TypeAnalyzer::new(
            Rc::clone(&symbol_table),
            Rc::clone(&symbol_locations),
            Rc::clone(&symbol_types),
            Rc::clone(&type_definitions),
        )));

        
        let expression_analyzer = Rc::new(RefCell::new(ExpressionAnalyzer::new(
            Rc::clone(&type_analyzer),
        )));
        
        let initializer_analyzer =  Rc::new(RefCell::new(InitializerAnalyzer::new(
            Rc::clone(&expression_analyzer),
            Rc::clone(&type_analyzer),
        )));
        
        let statement_analyzer = Rc::new(RefCell::new(StatementAnalyzer::new(
            None, // No current function initially
            Rc::clone(&expression_analyzer),
            Rc::clone(&initializer_analyzer)
        )));
        
        Self {
            symbol_table,
            current_function: None,
            symbol_locations,
            symbol_types,
            type_definitions,
            type_analyzer,
            statement_analyzer,
            expression_analyzer,
            initializer_analyzer,
        }
    }
    
    /// Analyze a translation unit
    pub fn analyze(&mut self, ast: &mut TranslationUnit) -> Result<(), CompilerError> {
        // First pass: collect all function and global variable declarations
        for item in &mut ast.items {
            match item {
                TopLevelItem::Function(func) => {
                    self.type_analyzer.borrow_mut().declare_function(func)?;
                }
                TopLevelItem::Declarations(decls) => {
                    for decl in decls {
                        // Handle different types of declarations explicitly
                        match decl.storage_class {
                            crate::StorageClass::Typedef => {
                                // Typedef defines a type alias
                                self.type_analyzer.borrow_mut().register_typedef(decl)?;
                            }
                            crate::StorageClass::Auto | 
                            crate::StorageClass::Static | 
                            crate::StorageClass::Extern | 
                            crate::StorageClass::Register => {
                                // These are actual variable declarations
                                // Note: Auto at file scope should be treated as Extern
                                self.type_analyzer.borrow_mut().declare_global_variable(decl)?;
                                
                                if let Some(initializer) = &mut decl.initializer {
                                    self.initializer_analyzer.borrow().analyze(initializer, &decl.decl_type)?;
                                }
                            }
                        }
                    }
                }
                TopLevelItem::TypeDefinition { name, type_def, .. } => {
                    self.type_analyzer.borrow_mut().register_type_definition(name.clone(), type_def.clone())?;
                }
            }
        }
        
        // Second pass: analyze function bodies
        for item in &mut ast.items {
            match item {
                TopLevelItem::Function(func) => {
                    self.analyze_function(func)?;
                }
                _ => {} // Already handled in first pass
            }
        }
        
        Ok(())
    }
    
    /// Analyze a function definition
    fn analyze_function(&mut self, func: &mut FunctionDefinition) -> Result<(), CompilerError> {
        // Set current function context
        self.current_function = Some(func.return_type.clone());
        
        // Enter function scope
        self.type_analyzer.borrow().symbol_table.borrow_mut().push_scope();
        
        // Add parameters to scope
        self.type_analyzer.borrow_mut().add_function_parameters(&mut func.parameters)?;
        
        // Analyze function body
        
        self.statement_analyzer.borrow().analyze_statement(&mut func.body)?;
        
        // Exit function scope
        self.type_analyzer.borrow().symbol_table.borrow_mut().pop_scope();
        self.current_function = None;
        
        Ok(())
    }
    
    /// Get both symbol types and type definitions
    pub fn into_type_info(self) -> Rc<RefCell<TypeAnalyzer>> {
        // (
        //     Rc::try_unwrap(self.symbol_types).ok().unwrap().into_inner(),
        //     Rc::try_unwrap(self.type_definitions).ok().unwrap().into_inner(),
        // )
        Rc::clone(&self.type_analyzer)
    }
    
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::{Frontend, TopLevelItem};

    #[test]
    fn test_semantic_analysis_simple_function() {
        let source = r#"
int main() {
    return 42;
}
"#;
        
        let mut ast = Frontend::parse_source(source).unwrap();
        let mut analyzer = SemanticAnalyzer::new();
        
        let result = analyzer.analyze(&mut ast);
        assert!(result.is_ok());
        
        // Check that function was analyzed
        match &ast.items[0] {
            TopLevelItem::Function(func) => {
                // Check that the return statement was analyzed
                match &func.body.kind {
                    StatementKind::Compound(statements) => {
                        match &statements[0].kind {
                            StatementKind::Return(Some(expr)) => {
                                assert_eq!(expr.expr_type, Some(Type::Int));
                            }
                            _ => panic!("Expected return statement"),
                        }
                    }
                    _ => panic!("Expected compound statement"),
                }
            }
            _ => panic!("Expected function definition"),
        }
    }

    #[test]
    fn test_semantic_analysis_undefined_variable() {
        let source = r#"
int main() {
    return x;  // undefined variable
}
"#;
        
        let mut ast = Frontend::parse_source(source).unwrap();
        let mut analyzer = SemanticAnalyzer::new();
        
        let result = analyzer.analyze(&mut ast);
        assert!(result.is_err());
        
        // Should be an undefined variable error
        match result.unwrap_err() {
            CompilerError::Semantic { message, .. } => {
                assert!(message.contains("Undefined variable"));
            }
            _ => panic!("Expected semantic error"),
        }
    }

    #[test]
    fn test_semantic_analysis_variable_declaration() {
        let source = r#"
int main() {
    int x = 42;
    return x;
}
"#;
        
        let mut ast = Frontend::parse_source(source).unwrap();
        let mut analyzer = SemanticAnalyzer::new();
        
        let result = analyzer.analyze(&mut ast);
        assert!(result.is_ok());
        
        // Variable should be resolved
        match &ast.items[0] {
            TopLevelItem::Function(func) => {
                match &func.body.kind {
                    StatementKind::Compound(statements) => {
                        // Check declaration
                        match &statements[0].kind {
                            StatementKind::Declaration { declarations } => {
                                assert!(declarations[0].symbol_id.is_some());
                            }
                            _ => panic!("Expected declaration statement"),
                        }
                        
                        // Check return statement uses the variable
                        match &statements[1].kind {
                            StatementKind::Return(Some(expr)) => {
                                match &expr.kind {
                                    ExpressionKind::Identifier { symbol_id, .. } => {
                                        assert!(symbol_id.is_some());
                                    }
                                    _ => panic!("Expected identifier in return"),
                                }
                            }
                            _ => panic!("Expected return statement"),
                        }
                    }
                    _ => panic!("Expected compound statement"),
                }
            }
            _ => panic!("Expected function definition"),
        }
    }
}