//! Function code generation

use std::collections::{HashMap, HashSet};
use crate::ir::{Value, IrBuilder, Module, IrType, FatPointer};
use crate::typed_ast::TypedFunction;
use crate::types::{Type, BankTag};
use crate::CompilerError;
use super::super::{VarInfo, statements::TypedStatementGenerator};
use super::utils::convert_type_default;
use crate::codegen::CodegenError;

fn is_aggregate(ty: &Type) -> bool {
    ty.is_aggregate()
}

/// Copy `size_words` words from `src_ptr` to `dst_ptr`.
fn copy_words(
    builder: &mut IrBuilder,
    src_ptr: Value,
    dst_ptr: Value,
    size_words: u64,
) -> Result<(), CompilerError> {
    for offset in 0..size_words {
        let offset_val = Value::Constant(offset as i64);
        let src_addr = builder.build_pointer_offset(src_ptr.clone(), offset_val.clone(), IrType::I16)?;
        let dst_addr = builder.build_pointer_offset(dst_ptr.clone(), offset_val, IrType::I16)?;
        let word = builder.build_load(src_addr, IrType::I16)?;
        builder.build_store(Value::Temp(word), dst_addr, IrType::I16)?;
    }
    Ok(())
}

/// Generate IR for a function
pub fn generate_function(
    builder: &mut IrBuilder,
    module: &mut Module,
    variables: &mut HashMap<String, VarInfo>,
    array_variables: &mut HashSet<String>,
    parameter_variables: &mut HashSet<String>,
    string_literals: &mut HashMap<String, String>,
    next_string_id: &mut u32,
    break_labels: &mut Vec<rcc_common::LabelId>,
    continue_labels: &mut Vec<rcc_common::LabelId>,
    func: &TypedFunction,
) -> Result<(), CompilerError> {
    // Save global variables before clearing
    let globals: Vec<_> = variables.iter()
        .filter(|(_, v)| matches!(v.value, Value::Global(_)))
        .map(|(k, v)| (k.clone(), v.clone()))
        .collect();
    
    // Save global array names
    let global_arrays: Vec<_> = array_variables.iter()
        .filter(|name| variables.get(*name)
            .map(|v| matches!(v.value, Value::Global(_)))
            .unwrap_or(false))
        .cloned()
        .collect();
    
    // Clear local state
    variables.clear();
    array_variables.clear();
    parameter_variables.clear();
    
    // Restore globals
    for (k, v) in globals {
        variables.insert(k, v);
    }
    
    // Restore global arrays
    for name in global_arrays {
        array_variables.insert(name);
    }
    
    // Convert return type
    let ret_type = convert_type_default(&func.return_type)?;
    
    // Create the function
    builder.create_function(func.name.clone(), ret_type);
    if func.is_variadic {
        builder.set_vararg(true);
    }
    
    // Create entry block
    let entry_label = builder.new_label();
    builder.create_block(entry_label)?;
    
    // First, add all parameters to the function.
    // Aggregates are passed as fat pointers to the caller's object (byval).
    // This must be done before any allocas to ensure temp IDs don't conflict.
    for (i, (_, param_type)) in func.parameters.iter().enumerate() {
        let ir_type = convert_type_default(param_type)?;
        let param_ir = if is_aggregate(param_type) {
            IrType::FatPtr(Box::new(ir_type))
        } else {
            ir_type
        };
        builder.add_parameter(i as rcc_common::TempId, param_ir);
    }
    
    // Now handle parameter storage (allocas and stores)
    for (i, (param_name, param_type)) in func.parameters.iter().enumerate() {
        let ir_type = convert_type_default(param_type)?;
        
        // Allocate space for parameter
        let param_addr = builder.build_alloca(ir_type.clone(), None)?;
        
        if is_aggregate(param_type) {
            // Caller passed a pointer to the original object. Copy into our local.
            let size_words = param_type.size_in_words().ok_or_else(|| CodegenError::InternalError {
                message: format!("Cannot determine size of aggregate parameter '{param_name}'"),
                location: rcc_common::SourceLocation::new_simple(0, 0),
            })?;
            let src_ptr = Value::FatPtr(FatPointer {
                addr: Box::new(Value::Temp(i as rcc_common::TempId)),
                bank: BankTag::Mixed,
            });
            copy_words(builder, src_ptr, param_addr.clone(), size_words)?;
        } else {
            // Store parameter value (parameters are passed as temporaries)
            let param_value = Value::Temp(i as rcc_common::TempId);
            builder.build_store(param_value, param_addr.clone(), ir_type.clone())?;
        }
        
        // Track the parameter
        let var_info = VarInfo {
            value: param_addr,
            ir_type,
            bank: Some(BankTag::Stack),
        };
        variables.insert(param_name.clone(), var_info);
        parameter_variables.insert(param_name.clone());
    }
    
    // Generate function body
    let mut switch_contexts = Vec::new();
    let mut stmt_gen = TypedStatementGenerator {
        builder,
        module,
        variables,
        array_variables,
        parameter_variables,
        string_literals,
        next_string_id,
        break_labels,
        continue_labels,
        switch_contexts: &mut switch_contexts,
        goto_labels: HashMap::new(),
        defined_goto_labels: HashSet::new(),
    };
    
    stmt_gen.generate(&func.body)?;
    stmt_gen.check_undefined_labels()?;
    
    // Add implicit return if needed
    if !builder.current_block_has_terminator() {
        if func.return_type == Type::Void {
            builder.build_return(None)?;
        } else if func.name == "main" {
            // C99: reaching the end of main is equivalent to return 0
            builder.build_return(Some(Value::Constant(0)))?;
        } else if builder.current_block_is_empty() {
            // Merge block after if/else where every branch already returned.
            builder.build_return(Some(Value::Constant(0)))?;
        } else {
            // A real fall-through (e.g. naked `asm("LOAD Rv0, ...")` with no
            // return) used to get a silent `return 0` that clobbered Rv0.
            return Err(CodegenError::MissingReturn {
                name: func.name.clone(),
                location: rcc_common::SourceLocation::new_simple(0, 0),
            }
            .into());
        }
    }
    
    // Finalize the function
    if let Some(final_function) = builder.finish_function() {
        module.add_function(final_function);
    }
    
    Ok(())
}