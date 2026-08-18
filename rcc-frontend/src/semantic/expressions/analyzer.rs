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
use crate::lexer::IntegerSuffix;

fn bits(value: i64) -> u64 {
    value as u64
}

fn fits_i16(value: i64) -> bool {
    bits(value) <= i16::MAX as u64
}

fn fits_u16(value: i64) -> bool {
    bits(value) <= u16::MAX as u64
}

fn fits_i32(value: i64) -> bool {
    bits(value) <= i32::MAX as u64
}

fn fits_u32(value: i64) -> bool {
    bits(value) <= u32::MAX as u64
}

fn fits_i64(value: i64) -> bool {
    bits(value) <= i64::MAX as u64
}

fn literal_integer_type(value: i64, suffix: IntegerSuffix, hex: bool) -> Type {
    match suffix {
        IntegerSuffix::None => {
            if fits_i16(value) {
                Type::Int
            } else if hex && fits_u16(value) {
                // C99 6.4.4.1: hex/octal try unsigned int before long.
                Type::UnsignedInt
            } else if fits_i32(value) {
                Type::Long
            } else if hex && fits_u32(value) {
                Type::UnsignedLong
            } else if fits_i64(value) {
                Type::LongLong
            } else {
                Type::UnsignedLongLong
            }
        }
        IntegerSuffix::Unsigned => {
            if fits_u16(value) {
                Type::UnsignedInt
            } else if fits_u32(value) {
                Type::UnsignedLong
            } else {
                Type::UnsignedLongLong
            }
        }
        IntegerSuffix::Long => {
            if fits_i32(value) {
                Type::Long
            } else if hex && fits_u32(value) {
                Type::UnsignedLong
            } else if fits_i64(value) {
                Type::LongLong
            } else {
                Type::UnsignedLongLong
            }
        }
        IntegerSuffix::UnsignedLong => {
            if fits_u32(value) {
                Type::UnsignedLong
            } else {
                Type::UnsignedLongLong
            }
        }
        IntegerSuffix::LongLong => {
            if fits_i64(value) {
                Type::LongLong
            } else {
                Type::UnsignedLongLong
            }
        }
        IntegerSuffix::UnsignedLongLong => Type::UnsignedLongLong,
    }
}

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
        let enum_val = if let ExpressionKind::Identifier { name, .. } = &expr.kind {
            self.type_analyzer.borrow().lookup_enum_constant(name)
        } else {
            None
        };
        if let Some(val) = enum_val {
            expr.kind = ExpressionKind::IntLiteral {
                value: val,
                suffix: crate::lexer::IntegerSuffix::None,
                hex: false,
            };
            expr.expr_type = Some(Type::Int);
            return Ok(());
        }

        let expr_type = match &mut expr.kind {
            ExpressionKind::IntLiteral { value: v, suffix, hex } => {
                // C99 6.4.4.1 on this ILP16 / 32-bit long / 64-bit long long target.
                literal_integer_type(*v, *suffix, *hex)
            }
            ExpressionKind::FloatLiteral { suffix, .. } => match suffix {
                crate::lexer::FloatSuffix::Float => Type::Float,
                crate::lexer::FloatSuffix::None | crate::lexer::FloatSuffix::LongDouble => Type::Double,
            },
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
                            is_variadic,
                        } => {
                            // Named parameters must be present; extras are allowed only
                            // for variadic functions.
                            if *is_variadic {
                                if arguments.len() < parameters.len() {
                                    return Err(SemanticError::ArgumentCountMismatch {
                                        expected: parameters.len(),
                                        found: arguments.len(),
                                        location: expr.span.start.clone(),
                                    }
                                    .into());
                                }
                            } else if arguments.len() != parameters.len() {
                                return Err(SemanticError::ArgumentCountMismatch {
                                    expected: parameters.len(),
                                    found: arguments.len(),
                                    location: expr.span.start.clone(),
                                }
                                .into());
                            }

                            // Check argument types with typedef awareness (named params only)
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
                Type::UnsignedInt // sizeof yields size_t (unsigned int)
            }

            ExpressionKind::SizeofType(_) => Type::UnsignedInt,

            ExpressionKind::CompoundLiteral {
                type_name,
                initializer,
            } => {
                // Analyze initializer expressions so identifiers get symbol IDs
                // while their declarations are still in scope (e.g. for-loop `i`).
                self.analyze_initializer_exprs(initializer)?;
                type_name.clone()
            }

            ExpressionKind::VaStart { ap, last } => {
                self.analyze(ap)?;
                self.analyze(last)?;
                Type::Void
            }

            ExpressionKind::VaArg { ap, arg_type } => {
                self.analyze(ap)?;
                arg_type.clone()
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
                if let Some(field_type) = crate::semantic::struct_layout::find_member_type(fields, member) {
                    Ok(self.type_analyzer.borrow().resolve_type(&field_type))
                } else {
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
                        if let Some(field_type) = crate::semantic::struct_layout::find_member_type(fields, member) {
                            Ok(self.type_analyzer.borrow().resolve_type(&field_type))
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