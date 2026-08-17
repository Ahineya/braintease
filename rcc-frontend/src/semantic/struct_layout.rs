//! Struct layout calculation
//! 
//! This module handles computing field offsets and total sizes for structs.
//! IMPORTANT: We explicitly error on unsupported features rather than silently
//! generating incorrect layouts.

use crate::types::{Type, StructField};
use crate::CompilerError;
use rcc_common::SourceLocation;
use std::collections::HashMap;

/// Information about a struct's memory layout
#[derive(Debug, Clone)]
pub struct StructLayout {
    pub fields: Vec<FieldLayout>,
    pub total_size: u64,  // Size in words
}

/// Layout information for a single field
#[derive(Debug, Clone)]
pub struct FieldLayout {
    pub name: String,
    pub field_type: Type,
    pub offset: u64,  // Offset in words from start of struct
    pub size: u64,    // Size in words
}

/// Calculate the memory layout for a struct
/// 
/// This function computes field offsets and total size.
/// Currently implements simple sequential layout without padding/alignment.
/// 
/// # Errors
/// Returns error if:
/// - A field has an incomplete type (e.g., void, unsized array)
/// - A field contains itself (recursive struct)
/// - The struct is too large (overflow)
pub fn calculate_struct_layout(
    fields: &[StructField],
    location: SourceLocation,
) -> Result<StructLayout, CompilerError> {
    calculate_struct_layout_with_defs(fields, location, None)
}

/// Calculate struct layout with access to type definitions for resolving named structs
pub fn calculate_struct_layout_with_defs(
    fields: &[StructField],
    location: SourceLocation,
    type_definitions: Option<&HashMap<String, Type>>,
) -> Result<StructLayout, CompilerError> {
    let mut layout_fields = Vec::new();
    let mut current_offset = 0u64;
    
    for (i, field) in fields.iter().enumerate() {
        let is_last = i + 1 == fields.len();
        let is_fam = matches!(field.field_type, Type::Array { size: None, .. });

        if is_fam {
            if !is_last {
                return Err(crate::semantic::SemanticError::IncompleteType {
                    type_name: format!("field '{}' has incomplete type {:?}",
                                     field.name, field.field_type),
                    location: location.clone(),
                }.into());
            }
            layout_fields.push(FieldLayout {
                name: field.name.clone(),
                field_type: field.field_type.clone(),
                offset: current_offset,
                size: 0,
            });
            continue;
        }

        // Get the size of this field
        let field_size = match get_type_size(&field.field_type, type_definitions) {
            Some(size) => size,
            None => {
                return Err(crate::semantic::SemanticError::IncompleteType {
                    type_name: format!("field '{}' has incomplete type {:?}", 
                                     field.name, field.field_type),
                    location: location.clone(),
                }.into());
            }
        };
        
        // Check for overflow
        let new_offset = current_offset.checked_add(field_size)
            .ok_or_else(|| crate::semantic::SemanticError::StructTooLarge {
                struct_name: "struct".to_string(),
                location: location.clone(),
            })?;
        
        // Add field to layout
        layout_fields.push(FieldLayout {
            name: field.name.clone(),
            field_type: field.field_type.clone(),
            offset: current_offset,
            size: field_size,
        });
        
        current_offset = new_offset;
    }
    
    Ok(StructLayout {
        fields: layout_fields,
        total_size: current_offset,
    })
}

/// Get the size of a type, resolving named struct references if needed
fn get_type_size(ty: &Type, type_definitions: Option<&HashMap<String, Type>>) -> Option<u64> {
    match ty {
        // For named struct references, look up the actual definition
        Type::Struct { name: Some(name), fields } if fields.is_empty() => {
            if let Some(defs) = type_definitions {
                if let Some(Type::Struct { fields: actual_fields, .. }) = defs.get(name) {
                    // Calculate size of the resolved struct
                    let mut total = 0u64;
                    for field in actual_fields {
                        if matches!(field.field_type, Type::Array { size: None, .. }) {
                            continue;
                        }
                        total += get_type_size(&field.field_type, type_definitions)?;
                    }
                    return Some(total);
                }
            }
            // If we can't resolve it, fall back to normal size calculation
            ty.size_in_words()
        }
        // For all other types, use the normal size calculation
        _ => ty.size_in_words()
    }
}

/// Find a field in a struct layout by name
pub fn find_field<'a>(
    layout: &'a StructLayout,
    field_name: &str,
) -> Option<&'a FieldLayout> {
    layout.fields.iter().find(|f| f.name == field_name)
}

/// Check if a type recursively contains itself
/// This is used to detect invalid recursive struct definitions
pub fn check_recursive_struct(
    ty: &Type,
    seen_types: &mut HashMap<String, bool>,
) -> bool {
    match ty {
        Type::Struct { name: Some(name), fields } => {
            // Check if we've seen this type before
            if let Some(&is_recursive) = seen_types.get(name) {
                return is_recursive;
            }
            
            // Mark as being checked (to handle mutual recursion)
            seen_types.insert(name.clone(), false);
            
            // Check each field
            for field in fields {
                // Direct containment (not via pointer) is recursive
                if !matches!(field.field_type, Type::Pointer { .. }) {
                    if let Type::Struct { name: Some(field_name), .. } = &field.field_type {
                        if field_name == name {
                            seen_types.insert(name.clone(), true);
                            return true;
                        }
                    }
                    
                    // Check nested structs
                    if check_recursive_struct(&field.field_type, seen_types) {
                        seen_types.insert(name.clone(), true);
                        return true;
                    }
                }
            }
            
            seen_types.insert(name.clone(), false);
            false
        }
        Type::Array { element_type, .. } => {
            // Arrays directly contain their element type
            check_recursive_struct(element_type, seen_types)
        }
        _ => false,
    }
}

