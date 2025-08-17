//! Type analysis and resolution
//! 
//! This module handles type checking, type resolution (typedef, struct references),
//! and type compatibility checks.

use std::cell::RefCell;
use crate::types::Type;
use crate::ast::{Expression, ExpressionKind, BinaryOp, UnaryOp, Parameter};
use std::collections::HashMap;
use std::rc::Rc;
use rcc_common::{CompilerError, SourceLocation, SymbolId, SymbolTable, StorageClass as CommonStorageClass};
use crate::{Declaration, FunctionDefinition, SemanticError, StorageClass};

/// Type context for semantic analysis
pub struct TypeAnalyzer {
    pub symbol_table: Rc<RefCell<SymbolTable>>,
    pub symbol_locations: Rc<RefCell<HashMap<SymbolId, SourceLocation>>>,
    pub symbol_types: Rc<RefCell<HashMap<SymbolId, Type>>>,
    pub type_definitions: Rc<RefCell<HashMap<String, Type>>>,
}

impl TypeAnalyzer {
    pub fn new(
        symbol_table: Rc<RefCell<SymbolTable>>,
        symbol_locations: Rc<RefCell<HashMap<SymbolId, SourceLocation>>>,
        symbol_types: Rc<RefCell<HashMap<SymbolId, Type>>>,
        type_definitions: Rc<RefCell<HashMap<String, Type>>>,
    ) -> Self {
        Self { 
            symbol_table,
            symbol_locations,
            symbol_types,
            type_definitions,
        }
    }
    
    /// Resolve only struct/union/enum references, preserving typedefs
    /// This is used when declaring variables to keep typedef information
    pub fn resolve_struct_references(&self, ty: &Type) -> Type {
        match ty {
            Type::Typedef(_) => {
                // Preserve typedef - don't resolve it
                ty.clone()
            }
            Type::Struct { name, fields } => {
                // Only resolve if it's a named struct with no fields (a reference)
                if let Some(name) = name {
                    if fields.is_empty() {
                        // This is a reference to a named struct type
                        if let Some(actual_type) = self.type_definitions.borrow().get(name) {
                            // Make sure we get a struct, not a typedef to a struct
                            if let Type::Struct { .. } = actual_type {
                                return actual_type.clone();
                            }
                        }
                    }
                }
                // Otherwise return as-is (already complete or anonymous)
                ty.clone()
            }
            Type::Union { name, fields } => {
                // Only resolve if it's a named union with no fields (a reference)
                if let Some(name) = name {
                    if fields.is_empty() {
                        // This is a reference to a named union type
                        if let Some(actual_type) = self.type_definitions.borrow().get(name) {
                            // Make sure we get a union, not a typedef to a union
                            if let Type::Union { .. } = actual_type {
                                return actual_type.clone();
                            }
                        }
                    }
                }
                // Otherwise return as-is (already complete or anonymous)
                ty.clone()
            }
            Type::Pointer { target, bank } => {
                // Recursively resolve struct references in pointed-to type
                Type::Pointer { 
                    target: Box::new(self.resolve_struct_references(target)),
                    bank: *bank,
                }
            }
            Type::Array { element_type, size } => {
                // Recursively resolve struct references in element type
                Type::Array {
                    element_type: Box::new(self.resolve_struct_references(element_type)),
                    size: *size,
                }
            }
            // Other types don't need resolution
            _ => ty.clone(),
        }
    }
    
    /// Resolve a type reference (e.g., struct Point -> actual struct definition)
    /// Returns the resolved type, or the original type if it cannot be resolved
    pub fn resolve_type(&self, ty: &Type) -> Type {
        match ty {
            Type::Typedef(name) => {
                // Look up typedef name
                if let Some(actual_type) = self.type_definitions.borrow().get(name) {
                    // Recursively resolve in case of typedef chains
                    self.resolve_type(actual_type)
                } else {
                    // Unresolved typedef - return as-is and let caller handle it
                    ty.clone()
                }
            }
            Type::Struct { name, fields } => {
                // Only resolve if it's a named struct with no fields (a reference)
                if let Some(name) = name {
                    if fields.is_empty() {
                        // This is a reference to a named struct type
                        if let Some(actual_type) = self.type_definitions.borrow().get(name) {
                            return actual_type.clone();
                        } else {
                            // Unresolved struct reference - incomplete type
                            // Return as-is to preserve the name for error reporting
                            // The caller should check for incomplete types
                            
                            // Todo: Investigate this shit
                            
                            return ty.clone();
                        }
                    }
                }
                // Otherwise return as-is (already complete or anonymous)
                ty.clone()
            }
            Type::Union { name, fields } => {
                // Only resolve if it's a named union with no fields (a reference)
                if let Some(name) = name {
                    if fields.is_empty() {
                        // This is a reference to a named union type
                        if let Some(actual_type) = self.type_definitions.borrow().get(name) {
                            return actual_type.clone();
                        } else {
                            // Unresolved union reference - incomplete type
                            // Return as-is to preserve the name for error reporting
                            // The caller should check for incomplete types
                            return ty.clone();
                        }
                    }
                }
                // Otherwise return as-is (already complete or anonymous)
                ty.clone()
            }
            Type::Pointer { target, bank } => {
                // Recursively resolve pointed-to type
                Type::Pointer { 
                    target: Box::new(self.resolve_type(target)),
                    bank: *bank,
                }
            }
            Type::Array { element_type, size } => {
                // Recursively resolve element type
                Type::Array {
                    element_type: Box::new(self.resolve_type(element_type)),
                    size: *size,
                }
            }
            // Basic types that don't need resolution
            Type::Void | Type::Bool | Type::Char | Type::SignedChar | Type::UnsignedChar |
            Type::Short | Type::UnsignedShort | Type::Int | Type::UnsignedInt |
            Type::Long | Type::UnsignedLong | Type::Error => ty.clone(),
            
            // Function types don't need resolution (parameters are already resolved)
            Type::Function { .. } => ty.clone(),
            
            // Enum types don't need resolution
            Type::Enum { .. } => ty.clone(),
        }
    }
    
    /// Get the common type for two operands (for arithmetic operations)
    pub fn arithmetic_result_type(&self, left: &Type, right: &Type) -> Type {
        // Simplified type promotion rules
        match (left, right) {
            (Type::Long, _) | (_, Type::Long) => Type::Long,
            (Type::UnsignedLong, _) | (_, Type::UnsignedLong) => Type::UnsignedLong,
            (Type::UnsignedInt, _) | (_, Type::UnsignedInt) => Type::UnsignedInt,
            _ => Type::Int,
        }
    }
    
    /// Get common type between two types
    pub fn common_type(&self, left: &Type, right: &Type) -> Type {
        if left == right {
            left.clone()
        } else if self.is_assignable(left, right) {
            left.clone()
        } else if self.is_assignable(right, left) {
            right.clone()
        } else {
            // Use arithmetic promotion rules
            self.arithmetic_result_type(left, right)
        }
    }
    
    /// Check if a type is unresolved (e.g., undefined typedef or incomplete struct)
    pub fn is_unresolved_type(&self, ty: &Type) -> bool {
        match ty {
            Type::Typedef(name) => {
                // A typedef is unresolved if it's not in our definitions
                !self.type_definitions.borrow().contains_key(name)
            }
            Type::Struct { name: Some(name), fields } if fields.is_empty() => {
                // A named struct reference is unresolved if not in definitions
                !self.type_definitions.borrow().contains_key(name)
            }
            Type::Union { name: Some(name), fields } if fields.is_empty() => {
                // A named union reference is unresolved if not in definitions
                !self.type_definitions.borrow().contains_key(name)
            }
            _ => false,
        }
    }
    
    /// Check if an expression is a valid lvalue
    pub fn is_lvalue(expr: &Expression) -> bool {
        match &expr.kind {
            ExpressionKind::Identifier { .. } => true,
            ExpressionKind::Unary { op: UnaryOp::Dereference, .. } => true,
            ExpressionKind::Binary { op: BinaryOp::Index, .. } => true,
            ExpressionKind::Member { .. } => true,
            _ => false,
        }
    }
    
    /// Check if a type is an integer (resolving typedefs)
    pub fn is_integer(&self, ty: &Type) -> bool {
        let resolved = self.resolve_type(ty);
        matches!(resolved, 
            Type::Bool | Type::Char | Type::SignedChar | Type::UnsignedChar |
            Type::Short | Type::UnsignedShort | Type::Int | Type::UnsignedInt |
            Type::Long | Type::UnsignedLong | Type::Enum { .. }
        )
    }
    
    /// Check if a type is a pointer (resolving typedefs)
    pub fn is_pointer(&self, ty: &Type) -> bool {
        let resolved = self.resolve_type(ty);
        matches!(resolved, Type::Pointer { .. } | Type::Array { .. })
    }
    
    /// Get pointer target type (resolving typedefs)
    pub fn pointer_target(&self, ty: &Type) -> Option<Type> {
        let resolved = self.resolve_type(ty);
        match resolved {
            Type::Pointer { target, .. } => Some(*target),
            Type::Array { element_type, .. } => Some(*element_type),
            _ => None,
        }
    }
    
    /// Check if two types are compatible for assignment
    pub fn is_assignable(&self, to: &Type, from: &Type) -> bool {
        // Special cases first
        if to == from {
            return true;
        }
        
        // Check if one is a typedef and the other is its underlying type
        match (to, from) {
            (Type::Typedef(name), other) | (other, Type::Typedef(name)) => {
                if let Some(resolved) = self.type_definitions.borrow().get(name) {
                    return self.is_assignable(resolved, other) || 
                           self.is_assignable(other, resolved);
                }
            }
            _ => {}
        }
        
        // Resolve both types for comparison
        let to_resolved = self.resolve_type(to);
        let from_resolved = self.resolve_type(from);
        
        match (&to_resolved, &from_resolved) {
            // Exact match after resolution
            (a, b) if a == b => true,
            
            // Struct types with same name are compatible
            (Type::Struct { name: Some(name1), .. }, Type::Struct { name: Some(name2), .. }) 
                if name1 == name2 => true,
            
            // Integer conversions
            (a, b) if self.is_integer(a) && self.is_integer(b) => true,
            
            // Pointer conversions
            (Type::Pointer { target: a, .. }, Type::Pointer { target: b, .. }) => {
                // void* is compatible with any pointer
                matches!(a.as_ref(), Type::Void) || matches!(b.as_ref(), Type::Void) 
                    || a.as_ref() == b.as_ref()
                    || self.is_assignable(a, b)
            }
            
            // Array to pointer decay
            (Type::Pointer { target, .. }, Type::Array { element_type, .. }) => {
                target.as_ref() == element_type.as_ref()
            }
            
            // Function to function pointer decay
            (Type::Pointer { target, .. }, func @ Type::Function { .. }) => {
                target.as_ref() == func
            }
            
            _ => false,
        }
    }
    
    /// Check if two function types are compatible
    pub fn is_compatible_function(&self, f1: &Type, f2: &Type) -> bool {
        let resolved1 = self.resolve_type(f1);
        let resolved2 = self.resolve_type(f2);
        
        match (&resolved1, &resolved2) {
            (
                Type::Function { return_type: ret1, parameters: params1, is_variadic: var1 },
                Type::Function { return_type: ret2, parameters: params2, is_variadic: var2 }
            ) => {
                // Variadic status must match
                if var1 != var2 {
                    return false;
                }
                
                // Return types must be compatible
                if !self.is_assignable(ret1, ret2) {
                    return false;
                }
                
                // Handle special case: () vs (void)
                let params1_is_void = params1.len() == 1 && matches!(params1[0], Type::Void);
                let params2_is_void = params2.len() == 1 && matches!(params2[0], Type::Void);
                let params1_is_empty = params1.is_empty();
                let params2_is_empty = params2.is_empty();
                
                if (params1_is_void || params1_is_empty) && (params2_is_void || params2_is_empty) {
                    return true;
                }
                
                // Otherwise, parameter counts must match
                if params1.len() != params2.len() {
                    return false;
                }
                
                // All parameter types must be compatible
                for (p1, p2) in params1.iter().zip(params2.iter()) {
                    if !self.is_assignable(p1, p2) {
                        return false;
                    }
                }
                
                true
            }
            _ => false,
        }
    }

    pub fn declare_function(&mut self, func: &FunctionDefinition) -> Result<(), CompilerError> {
        let func_type = Type::Function {
            return_type: Box::new(func.return_type.clone()),
            parameters: func.parameters.iter().map(|p| p.param_type.clone()).collect(),
            is_variadic: false, // TODO: Handle variadic functions
        };

        // Check if symbol already exists
        if self.symbol_table.borrow_mut().exists_in_current_scope(&func.name) {
            // Get the existing symbol's type
            if let Some(existing_symbol_id) = self.symbol_table.borrow().lookup(&func.name) {
                if let Some(existing_type) = self.symbol_types.borrow().get(&existing_symbol_id) {
                    // Check if the existing type is a compatible function declaration
                    if self.is_compatible_function(existing_type, &func_type) {
                        // Compatible function definition after declaration
                        // Update the symbol to mark it as defined
                        if let Some(symbol) = self.symbol_table.borrow_mut().get_symbol_mut(existing_symbol_id) {
                            *symbol = symbol.clone()
                                .as_function()
                                .as_defined()
                                .with_storage_class(
                                    match func.storage_class {
                                        StorageClass::Static => CommonStorageClass::Static,
                                        StorageClass::Extern => CommonStorageClass::Extern,
                                        _ => CommonStorageClass::Auto,
                                    }
                                );
                        }
                        return Ok(());
                    } else {
                        // Incompatible function redefinition
                        return Err(CompilerError::semantic_error(
                            format!(
                                "Function '{}' redefined with incompatible type: previous type was '{}', new type is '{}'",
                                func.name, existing_type, func_type
                            ),
                            func.span.start.clone(),
                        ));
                    }
                }
            }

            // If we get here, it's a non-function symbol being redefined as a function
            return Err(SemanticError::RedefinedSymbol {
                name: func.name.clone(),
                original_location: SourceLocation::new_simple(0, 0), // TODO: Track original location
                redefinition_location: func.span.start.clone(),
            }.into());
        }

        // New function definition
        let symbol_id = self.symbol_table.borrow_mut().add_symbol(func.name.clone());
        self.symbol_locations.borrow_mut().insert(symbol_id, func.span.start.clone());
        // Store the function type
        self.symbol_types.borrow_mut().insert(symbol_id, func_type);

        if let Some(symbol) = self.symbol_table.borrow_mut().get_symbol_mut(symbol_id) {
            *symbol = symbol.clone()
                .as_function()
                .as_defined()
                .with_storage_class(
                    match func.storage_class {
                        StorageClass::Static => CommonStorageClass::Static,
                        StorageClass::Extern => CommonStorageClass::Extern,
                        _ => CommonStorageClass::Auto,
                    }
                );
        }

        Ok(())
    }

    /// Register a typedef
    pub fn register_typedef(&mut self, decl: &mut Declaration) -> Result<(), CompilerError> {
        // Resolve the type first
        decl.decl_type = self.resolve_type(&decl.decl_type);
        // Register this as a type definition
        self.type_definitions.borrow_mut().insert(decl.name.clone(), decl.decl_type.clone());
        Ok(())
    }

    /// Declare a global variable in the symbol table
    pub fn declare_global_variable(&mut self, decl: &mut Declaration) -> Result<(), CompilerError> {
        // Validate that this is actually a variable declaration
        match decl.storage_class {
            StorageClass::Auto | StorageClass::Static |
            StorageClass::Extern | StorageClass::Register => {
                // These are valid storage classes for variables
            }
            StorageClass::Typedef => {
                return Err(CompilerError::internal_error(
                    format!("Typedef '{}' should not be processed as global variable", decl.name)
                ));
            }
        }

        // Check if symbol already exists
        if self.symbol_table.borrow().exists_in_current_scope(&decl.name) {
            // For function declarations, allow redeclaration if types are compatible
            if let Type::Function { .. } = &decl.decl_type {
                // Get the existing symbol's type
                if let Some(existing_symbol_id) = self.symbol_table.borrow().lookup(&decl.name) {
                    if let Some(existing_type) = self.symbol_types.borrow().get(&existing_symbol_id) {
                        // Check if the existing type is also a function and if they're compatible
                        if self.is_compatible_function(existing_type, &decl.decl_type) {
                            // Compatible function redeclaration - skip adding to symbol table
                            // but still return Ok to indicate this is valid
                            return Ok(());
                        } else {
                            // Incompatible function redeclaration
                            return Err(CompilerError::semantic_error(
                                format!(
                                    "Incompatible redeclaration of function '{}': previous type was '{}', new type is '{}'",
                                    decl.name, existing_type, decl.decl_type
                                ),
                                decl.span.start.clone(),
                            ));
                        }
                    }
                }
            }

            return Err(SemanticError::RedefinedSymbol {
                name: decl.name.clone(),
                original_location: SourceLocation::new_simple(0, 0), // TODO: Track original location
                redefinition_location: decl.span.start.clone(),
            }.into());
        }

        // Resolve the type (in case it references a named struct/union/enum)
        decl.decl_type = self.resolve_type(&decl.decl_type);

        // Fix storage class for global variables
        // If storage class is Auto (the default), change it to Extern for globals
        if decl.storage_class == StorageClass::Auto {
            decl.storage_class = StorageClass::Extern;
        }

        let symbol_id = self.symbol_table.borrow_mut().add_symbol(decl.name.clone());
        self.symbol_locations.borrow_mut().insert(symbol_id, decl.span.start.clone());
        // Store the global variable type (keeping typedef if present)
        self.symbol_types.borrow_mut().insert(symbol_id, decl.decl_type.clone());

        if let Some(symbol) = self.symbol_table.borrow_mut().get_symbol_mut(symbol_id) {
            *symbol = symbol.clone()
                .with_storage_class(
                    match decl.storage_class {
                        StorageClass::Static => CommonStorageClass::Static,
                        StorageClass::Extern => CommonStorageClass::Extern,
                        StorageClass::Register => CommonStorageClass::Register,
                        _ => CommonStorageClass::Extern,  // Default to Extern for globals
                    }
                );
        }

        Ok(())
    }

    /// Register a type definition (struct, union, enum)
    pub fn register_type_definition(&mut self, name: String, mut type_def: Type) -> Result<(), CompilerError> {
        // Check if type already exists
        if self.type_definitions.borrow().contains_key(&name) {
            return Err(SemanticError::RedefinedType {
                name: name.clone(),
            }.into());
        }

        // Resolve field types in struct/union definitions
        match &mut type_def {
            Type::Struct { fields, .. } => {
                for field in fields.iter_mut() {
                    field.field_type = self.resolve_type(&field.field_type);
                }
            }
            Type::Union { fields, .. } => {
                for field in fields.iter_mut() {
                    field.field_type = self.resolve_type(&field.field_type);
                }
            }
            _ => {}
        }

        // Store the type definition with resolved field types
        self.type_definitions.borrow_mut().insert(name, type_def);

        Ok(())
    }

    /// Add function parameters to the symbol table
    pub fn add_function_parameters(&mut self, parameters: &mut [Parameter]) -> Result<(), CompilerError> {
        for param in parameters {
            if let Some(name) = &param.name {
                let symbol_id = self.symbol_table.borrow_mut().add_symbol(name.clone());
                param.symbol_id = Some(symbol_id);
                self.symbol_locations.borrow_mut().insert(symbol_id, param.span.start.clone());
                // Store the parameter type (keeping typedef if present)
                self.symbol_types.borrow_mut().insert(symbol_id, param.param_type.clone());
            }
        }
        Ok(())
    }
    
}