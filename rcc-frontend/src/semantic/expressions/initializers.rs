//! Initializer and compound literal analysis

use std::cell::RefCell;
use crate::ast::*;
use crate::semantic::errors::SemanticError;
use crate::Type;
use rcc_common::{CompilerError};
use std::rc::Rc;
use crate::semantic::expressions::ExpressionAnalyzer;
use crate::semantic::types::TypeAnalyzer;

pub struct InitializerAnalyzer {
    expression_analyzer: Rc<RefCell<ExpressionAnalyzer>>,
    type_analyzer: Rc<RefCell<TypeAnalyzer>>
}

impl InitializerAnalyzer {
    pub fn new(
        expression_analyzer: Rc<RefCell<ExpressionAnalyzer>>,
        type_analyzer: Rc<RefCell<TypeAnalyzer>>) -> Self {
        Self {
            expression_analyzer,
            type_analyzer
        }
    }
    
    /// Analyze an initializer
    pub fn analyze(
        &self,
        init: &mut Initializer,
        expected_type: &Type,
    ) -> Result<(), CompilerError>
    {
        let resolved = self.type_analyzer.borrow().resolve_type(expected_type);

        match &mut init.kind {
            InitializerKind::Expression(expr) => {
                self.expression_analyzer.borrow().analyze(expr)?;
        
                // Check type compatibility with typedef awareness
                if let Some(expr_type) = &expr.expr_type {
                    // Special case: Allow 0 to initialize pointers (NULL)
                    let is_null_init = matches!(resolved, Type::Pointer { .. })
                        && self.type_analyzer.borrow().is_integer(expr_type)
                        && matches!(expr.kind, ExpressionKind::IntLiteral(0));
                    
                    // Use typedef-aware type compatibility checking
                    if !is_null_init && !self.type_analyzer.borrow().is_assignable(&resolved, expr_type) {
                        log::debug!("Type mismatch in initializer: expected={:?}, found={:?}", expected_type, expr_type);
                        return Err(SemanticError::TypeMismatch {
                            expected: expected_type.clone(),
                            found: expr_type.clone(),
                            location: expr.span.start.clone(),
                        }
                        .into());
                    }
                }
                
                Ok(())
            }
        
            InitializerKind::List(initializers) => {
                match &resolved {
                    Type::Array { element_type, .. } => {
                        for item in initializers {
                            let is_designated = matches!(item.kind, InitializerKind::Designated { .. });
                            let is_member = matches!(
                                item.kind,
                                InitializerKind::Designated { designator: Designator::Member(_), .. }
                            );
                            if is_member {
                                return Err(SemanticError::TypeMismatch {
                                    expected: resolved.clone(),
                                    found: Type::Error,
                                    location: item.span.start.clone(),
                                }.into());
                            } else if is_designated {
                                self.analyze(item, &resolved)?;
                            } else {
                                self.analyze(item, element_type)?;
                            }
                        }
                        Ok(())
                    }
                    Type::Struct { fields, .. } | Type::Union { fields, .. } => {
                        let mut current = 0usize;
                        for item in initializers {
                            let designated_member = match &item.kind {
                                InitializerKind::Designated { designator: Designator::Member(name), .. } => {
                                    Some(name.clone())
                                }
                                InitializerKind::Designated { .. } => Some(String::new()),
                                _ => None,
                            };
                            if let Some(name) = designated_member {
                                self.analyze(item, &resolved)?;
                                if !name.is_empty() {
                                    if let Some(idx) = fields.iter().position(|f| f.name == name) {
                                        current = idx + 1;
                                    }
                                }
                            } else {
                                if current >= fields.len() {
                                    return Err(SemanticError::TypeMismatch {
                                        expected: resolved.clone(),
                                        found: Type::Error,
                                        location: item.span.start.clone(),
                                    }.into());
                                }
                                self.analyze(item, &fields[current].field_type)?;
                                current += 1;
                            }
                        }
                        Ok(())
                    }
                    _ => {
                        Err(SemanticError::TypeMismatch {
                            expected: expected_type.clone(),
                            found: Type::Error,
                            location: init.span.start.clone(),
                        }
                        .into())
                    }
                }
            }
        
            InitializerKind::Designated { designator, initializer } => {
                let designator = designator.clone();
                match (&resolved, &designator) {
                    (Type::Struct { fields, name, .. } | Type::Union { fields, name, .. }, Designator::Member(member)) => {
                        if let Some(field) = fields.iter().find(|f| f.name == *member) {
                            let field_type = field.field_type.clone();
                            self.analyze(initializer, &field_type)
                        } else {
                            let struct_name = name.clone().unwrap_or_else(|| format!("{resolved}"));
                            Err(SemanticError::UndefinedMember {
                                struct_name,
                                member_name: member.clone(),
                                location: init.span.start.clone(),
                            }.into())
                        }
                    }
                    (Type::Array { element_type, .. }, Designator::Index(_)) => {
                        let elem = element_type.clone();
                        self.analyze(initializer, &elem)
                    }
                    _ => {
                        Err(SemanticError::TypeMismatch {
                            expected: expected_type.clone(),
                            found: Type::Error,
                            location: init.span.start.clone(),
                        }.into())
                    }
                }
            }
        }
    }
}
