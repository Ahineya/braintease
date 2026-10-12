//! Literal expression code generation

use super::TypedExpressionGenerator;
use crate::ir::{GlobalVariable, Linkage, Value, FatPointer, IrType};
use crate::types::BankTag;
use crate::CompilerError;

pub fn generate_string_literal(
    gen: &mut TypedExpressionGenerator,
    s: &str,
) -> Result<Value, CompilerError> {
    // Create a unique name for this string literal
    let string_id = *gen.next_string_id;
    *gen.next_string_id += 1;
    
    // Create a simple, readable name for the string literal
    let name = format!("__str_{string_id}");
    
    // Convert string bytes to array of constants (including null terminator)
    let mut char_values: Vec<i64> = s.bytes().map(|b| b as i64).collect();
    char_values.push(0); // Add null terminator
    
    let global = GlobalVariable {
        name: name.clone(),
        var_type: IrType::Array {
            element_type: Box::new(IrType::I16),
            size: char_values.len() as u64,
        },
        is_constant: true, // String literals are constant
        initializer: Some(Value::ConstantArray(char_values)),
        linkage: Linkage::Internal,
        symbol_id: None,
    };
    
    gen.module.add_global(global);
    gen.string_literals
        .insert(name.clone(), s.to_string());
    
    // Return a fat pointer to the string
    Ok(Value::FatPtr(FatPointer {
        addr: Box::new(Value::Global(name)),
        bank: BankTag::Global,
    }))
}