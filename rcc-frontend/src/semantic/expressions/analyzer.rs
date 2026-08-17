//! Main expression analyzer that coordinates all expression analysis

use std::cell::RefCell;
use crate::ast::*;
use crate::semantic::errors::SemanticError;
use crate::semantic::types::TypeAnalyzer;
use crate::Type;
use rcc_common::CompilerError;
use std::rc::Rc;
use crate::semantic::expressions::initializers::InitializerAnalyzer;
use super::binary::BinaryOperationAnalyzer;
use super::unary::UnaryOperationAnalyzer;

pub struct ExpressionAnalyzer {
    pub type_analyzer: Rc<RefCell<TypeAnalyzer>>,
    pub initializer_analyzer: Option<Rc<RefCell<InitializerAnalyzer>>>
}

impl ExpressionAnalyzer {
    pub fn new(
        type_analyzer: Rc<RefCell<TypeAnalyzer>>,
    ) -> Self {
        Self {
            type_analyzer,
            initializer_analyzer: None,
        }
    }
    
    pub fn add_initializer_analyzer(
        &mut self,
        initializer_analyzer: Rc<RefCell<InitializerAnalyzer>>,
    ) {
        self.initializer_analyzer = Some(initializer_analyzer);
    }

    /// Analyze an expression and infer its type
    pub fn analyze(
        &self,
        expr: &mut Expression,
    ) -> Result<(), CompilerError> {
        let expr_type = match &mut expr.kind {
            ExpressionKind::IntLiteral(_) => Type::Int,
            ExpressionKind::CharLiteral(_) => Type::Char,
            ExpressionKind::StringLiteral(_) => Type::Array {
                element_type: Box::new(Type::Char),
                size: None, // TODO: Calculate string length
            },

            ExpressionKind::Identifier { name, symbol_id } => {
                // Look up in symbol table
                if let Some(id) = self.type_analyzer.borrow().symbol_table.borrow().lookup(name) {
                    *symbol_id = Some(id);
                    // Get the actual type from our type mapping
                    self.type_analyzer.borrow().symbol_types.borrow().get(&id).cloned().unwrap_or(Type::Int)
                } else {
                    return Err(SemanticError::UndefinedVariable {
                        name: name.clone(),
                        location: expr.span.start.clone(),
                    }
                    .into());
                }
            }

            ExpressionKind::Binary { op, left, right } => {
                self.analyze(left)?;
                self.analyze(right)?;

                let binary_analyzer = BinaryOperationAnalyzer::new(Rc::clone(&self.type_analyzer));
                binary_analyzer.analyze(*op, left, right)?
            }

            ExpressionKind::Unary { op, operand } => {
                self.analyze(operand)?;

                let unary_analyzer = UnaryOperationAnalyzer::new(Rc::clone(&self.type_analyzer));
                unary_analyzer.analyze(*op, operand)?
            }

            ExpressionKind::Call {
                function,
                arguments,
            } => {
                self.analyze(function)?;

                for arg in arguments.iter_mut() {
                    self.analyze(arg)?;
                }

                // Check if function is callable
                if let Some(func_type) = &function.expr_type {
                    // Dereference function pointer to get the actual function type
                    let callable_type = match func_type {
                        Type::Pointer { target, .. } => target.as_ref(),
                        other => other,
                    };
                    
                    match callable_type {
                        Type::Function {
                            return_type,
                            parameters,
                            ..
                        } => {
                            // Check argument count
                            if arguments.len() != parameters.len() {
                                return Err(SemanticError::ArgumentCountMismatch {
                                    expected: parameters.len(),
                                    found: arguments.len(),
                                    location: expr.span.start.clone(),
                                }
                                .into());
                            }

                            // Check argument types with typedef awareness
                            for (arg, param_type) in arguments.iter().zip(parameters.iter()) {
                                if let Some(arg_type) = &arg.expr_type {
                                    
                                    if !self.type_analyzer.borrow().is_assignable(param_type, arg_type) {
                                        return Err(SemanticError::TypeMismatch {
                                            expected: param_type.clone(),
                                            found: arg_type.clone(),
                                            location: arg.span.start.clone(),
                                        }
                                        .into());
                                    }
                                }
                            }

                            *return_type.clone()
                        }
                        _ => {
                            return Err(SemanticError::InvalidFunctionCall {
                                function_type: func_type.clone(),
                                location: expr.span.start.clone(),
                            }
                            .into());
                        }
                    }
                } else {
                    Type::Error
                }
            }

            ExpressionKind::Member {
                object,
                member,
                is_pointer,
            } => {
                self.analyze(object)?;

                // Get the struct type from the object
                let struct_type = if *is_pointer {
                    // For arrow operator, dereference the pointer first
                    if let Some(obj_type) = &object.expr_type {
                        if let Some(target) = obj_type.pointer_target() {
                            target.clone()
                        } else {
                            return Err(SemanticError::InvalidOperation {
                                operation: "arrow operator on non-pointer".to_string(),
                                operand_type: obj_type.clone(),
                                location: object.span.start.clone(),
                            }
                            .into());
                        }
                    } else {
                        Type::Error
                    }
                } else {
                    // For dot operator, use the object type directly
                    object.expr_type.as_ref().unwrap_or(&Type::Error).clone()
                };

                // Look up the field type in the struct
                self.analyze_member_access(&struct_type, member, &expr.span.start)?
            }

            ExpressionKind::Conditional {
                condition,
                then_expr,
                else_expr,
            } => {
                self.analyze(condition)?;
                self.analyze(then_expr)?;
                self.analyze(else_expr)?;

                self.check_boolean_context(condition)?;

                // Result type is the common type of then and else expressions
                if let (Some(then_type), Some(else_type)) =
                    (&then_expr.expr_type, &else_expr.expr_type)
                {
                    self.type_analyzer.borrow().common_type(then_type, else_type)
                } else {
                    Type::Error
                }
            }

            ExpressionKind::Cast {
                target_type,
                operand,
            } => {
                self.analyze(operand)?;
                target_type.clone()
            }

            ExpressionKind::SizeofExpr(operand) => {
                self.analyze(operand)?;
                Type::UnsignedLong // sizeof returns size_t, which is unsigned long on Ripple
            }

            ExpressionKind::SizeofType(_) => Type::UnsignedLong,

            ExpressionKind::CompoundLiteral {
                type_name,
                initializer,
            } => {
                // Analyze initializer expressions so identifiers get symbol IDs
                // while their declarations are still in scope (e.g. for-loop `i`).
                self.analyze_initializer_exprs(initializer)?;
                type_name.clone()
            }
        };

        expr.expr_type = Some(expr_type);
        Ok(())
    }

    fn analyze_initializer_exprs(&self, init: &mut Initializer) -> Result<(), CompilerError> {
        match &mut init.kind {
            InitializerKind::Expression(expr) => self.analyze(expr),
            InitializerKind::List(items) => {
                for item in items {
                    self.analyze_initializer_exprs(item)?;
                }
                Ok(())
            }
            InitializerKind::Designated { initializer, .. } => {
                self.analyze_initializer_exprs(initializer)
            }
        }
    }

    /// Check if expression is used in boolean context and can be converted
    pub fn check_boolean_context(&self, expr: &Expression) -> Result<(), CompilerError> {
        // In C, any scalar type can be used in boolean context
        if let Some(expr_type) = &expr.expr_type {
            match expr_type {
                Type::Void => Err(SemanticError::InvalidOperation {
                    operation: "boolean conversion".to_string(),
                    operand_type: expr_type.clone(),
                    location: expr.span.start.clone(),
                }
                .into()),
                _ => Ok(()),
            }
        } else {
            Ok(())
        }
    }

    /// Analyze member access on a struct type
    fn analyze_member_access(
        &self,
        struct_type: &Type,
        member: &str,
        location: &rcc_common::SourceLocation,
    ) -> Result<Type, CompilerError> {
        // First resolve the type if it's a struct reference
        let resolved_type = self.type_analyzer.borrow().resolve_type(struct_type);
        
        match &resolved_type {
            Type::Struct { fields, name, .. } | Type::Union { fields, name, .. } => {
                // Find the field by name
                if let Some(field) = fields.iter().find(|f| f.name == member) {
                    // Resolve the field type in case it's a struct reference
                    Ok(self.type_analyzer.borrow().resolve_type(&field.field_type))
                } else {
                    // Use the actual struct name if available, otherwise use a generic description
                    let struct_name = name
                        .clone()
                        .unwrap_or_else(|| format!("{struct_type}"));
                    Err(SemanticError::UndefinedMember {
                        struct_name,
                        member_name: member.to_string(),
                        location: location.clone(),
                    }
                    .into())
                }
            }
            Type::Typedef(name) => {
                // Look up the typedef in type definitions
                let resolved_type = self.type_analyzer.borrow().resolve_type(&Type::Typedef(name.clone()));

                // Check if the type was actually resolved
                if let Type::Typedef(unresolved_name) = &resolved_type {
                    return Err(SemanticError::UndefinedType {
                        name: unresolved_name.clone(),
                        location: location.clone(),
                    }
                    .into());
                }

                match &resolved_type {
                    Type::Struct { fields, .. } | Type::Union { fields, .. } => {
                        if let Some(field) = fields.iter().find(|f| f.name == member) {
                            // Resolve the field type in case it's a struct reference
                            Ok(self.type_analyzer.borrow().resolve_type(&field.field_type))
                        } else {
                            Err(SemanticError::UndefinedMember {
                                struct_name: name.clone(),
                                member_name: member.to_string(),
                                location: location.clone(),
                            }
                            .into())
                        }
                    }
                    _ => Err(SemanticError::InvalidOperation {
                        operation: "member access on non-struct/union".to_string(),
                        operand_type: resolved_type,
                        location: location.clone(),
                    }
                    .into()),
                }
            }
            _ => Err(SemanticError::InvalidOperation {
                operation: "member access on non-struct/union".to_string(),
                operand_type: struct_type.clone(),
                location: location.clone(),
            }
            .into()),
        }
    }
}