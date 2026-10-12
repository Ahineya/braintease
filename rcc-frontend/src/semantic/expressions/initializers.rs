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

    /// Complete an object's inferred array bound before later expressions look
    /// up its type (in particular sizeof, which must not decay the array).
    pub fn analyze_declaration(&self, decl: &mut Declaration) -> Result<(), CompilerError> {
        let Some(init) = &mut decl.initializer else { return Ok(()); };
        self.analyze(init, &decl.decl_type)?;
        let resolved = self.type_analyzer.borrow().resolve_type(&decl.decl_type);
        let Type::Array { element_type, size: None } = resolved else { return Ok(()); };
        let size = if let Some(text) = character_array_string(init, &element_type) {
            text.len() as u64 + 1
        } else if let InitializerKind::List(items) = &init.kind {
            let mut next = 0u64;
            let mut length = 0u64;
            for item in items {
                if let InitializerKind::Designated { designator: Designator::Index(expr), .. } = &item.kind {
                    let index = crate::ast::const_eval::eval_integer_constant(expr).map_err(|error| {
                        CompilerError::semantic_error(error.0, expr.span.start.clone())
                    })?;
                    if index < 0 {
                        return Err(CompilerError::semantic_error(
                            "Array designator index must be non-negative".to_string(),
                            expr.span.start.clone(),
                        ));
                    }
                    next = index as u64;
                }
                next += 1;
                length = length.max(next);
            }
            length
        } else {
            return Ok(());
        };
        let complete = Type::Array { element_type, size: Some(size) };
        if let Some(id) = decl.symbol_id {
            let analyzer = self.type_analyzer.borrow();
            let previous = analyzer.symbol_types.borrow().get(&id).cloned();
            if let Some(previous) = previous {
                if !analyzer.types_compatible_for_object_redeclaration(&previous, &complete) {
                    return Err(CompilerError::semantic_error(
                        format!("Incompatible inferred array bound for '{}'", decl.name),
                        decl.span.start.clone(),
                    ));
                }
            }
            analyzer.symbol_types.borrow_mut().insert(id, complete.clone());
        }
        decl.decl_type = complete;
        Ok(())
    }
    
    /// Analyze an initializer
    pub fn analyze(
        &self,
        init: &mut Initializer,
        expected_type: &Type,
    ) -> Result<(), CompilerError>
    {
        let resolved = self.type_analyzer.borrow().resolve_type(expected_type);

        // A string (optionally enclosed in braces) initializes the complete
        // character array, rather than its first scalar element.
        if let Type::Array { element_type, size } = &resolved {
            if let Some(text) = character_array_string(init, element_type) {
                if size.is_some_and(|bound| bound < text.len() as u64) {
                    return Err(CompilerError::semantic_error(
                        "String initializer is too long for the character array".to_string(),
                        init.span.start.clone(),
                    ));
                }
                return Ok(());
            }
        }

        match &mut init.kind {
            InitializerKind::Expression(expr) => {
                self.expression_analyzer.borrow().analyze(expr)?;
                let target = match &resolved {
                    Type::Union { fields, .. } => fields.first().map(|f| &f.field_type).unwrap_or(&resolved),
                    _ => &resolved,
                };
                self.type_analyzer.borrow().convert_null_pointer_constant(target, expr);
        
                // Check type compatibility with typedef awareness
                if let Some(expr_type) = &expr.expr_type {
                    let expression_type = self.type_analyzer.borrow().resolve_type(expr_type);
                    let assign_to = match (&resolved, &expression_type) {
                        // A union expression initializes the union object as a whole.
                        (Type::Union { .. }, Type::Union { .. }) => &resolved,
                        (Type::Union { fields, .. }, _) => {
                            fields.first().map(|f| &f.field_type).unwrap_or(&resolved)
                        }
                        _ => &resolved,
                    };
                    // Special case: Allow 0 to initialize pointers (NULL)
                    let is_null_init = matches!(assign_to, Type::Pointer { .. })
                        && self.type_analyzer.borrow().is_integer(expr_type)
                        && matches!(expr.kind, ExpressionKind::IntLiteral { value: 0, .. });
                    
                    // Use typedef-aware type compatibility checking
                    if !is_null_init && !self.type_analyzer.borrow().is_assignable(assign_to, expr_type) {
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
                if let Designator::Index(expr) = designator {
                    self.expression_analyzer.borrow().analyze(expr)?;
                }
                let designator = designator.clone();
                match (&resolved, &designator) {
                    (Type::Struct { fields, name, .. } | Type::Union { fields, name, .. }, Designator::Member(member)) => {
                        if let Some(field_type) = crate::semantic::struct_layout::find_member_type(fields, member) {
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

/// C permits a character array's string initializer with or without braces.
pub(crate) fn character_array_string<'a>(init: &'a Initializer, element_type: &Type) -> Option<&'a str> {
    if !matches!(element_type, Type::Char | Type::SignedChar | Type::UnsignedChar) {
        return None;
    }
    match &init.kind {
        InitializerKind::Expression(expr) => match &expr.kind {
            ExpressionKind::StringLiteral(text) => Some(text),
            _ => None,
        },
        InitializerKind::List(items) if items.len() == 1 => match &items[0].kind {
            InitializerKind::Expression(expr) => match &expr.kind {
                ExpressionKind::StringLiteral(text) => Some(text),
                _ => None,
            },
            _ => None,
        },
        _ => None,
    }
}
