//! Miscellaneous statement code generation (expression statements, compound blocks, inline asm)

use super::TypedStatementGenerator;
use crate::typed_ast::{TypedStmt, TypedExpr, TypedAsmOperand};
use crate::ir::{AsmOperandIR, Value};
use crate::CompilerError;
use crate::codegen::expressions::generate_lvalue_address;
use crate::codegen::types::convert_type;
use rcc_common::SourceLocation;

pub fn generate_expression_stmt(
    gen: &mut TypedStatementGenerator,
    expr: &TypedExpr,
) -> Result<(), CompilerError> {
    let mut expr_gen = gen.create_expression_generator();
    expr_gen.generate(expr)?;
    Ok(())
}

pub fn generate_compound(
    gen: &mut TypedStatementGenerator,
    statements: &[TypedStmt],
) -> Result<(), CompilerError> {
    for stmt in statements {
        gen.generate(stmt)?;
    }
    Ok(())
}

pub fn generate_inline_asm(
    gen: &mut TypedStatementGenerator,
    assembly: &str,
) -> Result<(), CompilerError> {
    gen.builder.build_inline_asm(assembly.to_string())?;
    Ok(())
}

pub fn generate_inline_asm_extended(
    gen: &mut TypedStatementGenerator,
    assembly: &str,
    outputs: &[TypedAsmOperand],
    inputs: &[TypedAsmOperand],
    clobbers: &[String],
) -> Result<(), CompilerError> {
    
    // If no operands, use simple version
    if outputs.is_empty() && inputs.is_empty() {
        return generate_inline_asm(gen, assembly);
    }
    
    // Generate IR values for output operands
    let mut ir_outputs = Vec::new();
    let mut output_addresses = Vec::new(); // Store addresses for later storing
    let mut output_types = Vec::new(); // Store types for creating temps
    
    for (i, op) in outputs.iter().enumerate() {
        // Check if it's a read-write constraint (starts with '+')
        let is_read_write = op.constraint.starts_with('+');
        
        // For output operands, we need to:
        // 1. Generate the lvalue address
        // 2. Create a temp to hold the output value
        
        let mut expr_gen = gen.create_expression_generator();
        
        // Generate the address of the output operand (must be an lvalue)
        let addr = generate_lvalue_address(&mut expr_gen, &op.expr)?;
        output_addresses.push(addr.clone());
        
        // Get the type of the expression
        let expr_type = op.expr.get_type();
        let ir_type = convert_type(expr_type, SourceLocation::new_simple(0, 0))?;
        output_types.push(ir_type.clone());
        
        // Create a new temp for the output value
        let output_temp = gen.builder.new_temp();
        let output_value = Value::Temp(output_temp);
        
        ir_outputs.push(AsmOperandIR {
            constraint: op.constraint.clone(),
            value: output_value,
            tied_to: if is_read_write { Some(inputs.len() + i) } else { None },
        });
    }
    
    // Generate IR values for input operands  
    let mut ir_inputs = Vec::new();
    for op in inputs {
        let mut expr_gen = gen.create_expression_generator();
        let value = expr_gen.generate(&op.expr)?;
        
        ir_inputs.push(AsmOperandIR {
            constraint: op.constraint.clone(),
            value,
            tied_to: None,
        });
    }
    
    // Add read-write operands as inputs too (they appear in both lists)
    for (i, op) in outputs.iter().enumerate() {
        if op.constraint.starts_with('+') {
            // For read-write, load the current value from the address
            let current_value = gen.builder.build_load(
                output_addresses[i].clone(),
                output_types[i].clone()
            )?;
            
            ir_inputs.push(AsmOperandIR {
                constraint: op.constraint[1..].to_string(), // Remove '+' prefix
                value: Value::Temp(current_value),
                tied_to: Some(i), // Tied to output operand i
            });
        }
    }
    
    // Build the inline assembly instruction
    gen.builder.build_inline_asm_extended(
        assembly.to_string(),
        ir_outputs.clone(),
        ir_inputs,
        clobbers.to_vec(),
    )?;
    
    // After the inline assembly, store the output values back to their addresses
    for (i, output) in ir_outputs.iter().enumerate() {
        gen.builder.build_store(
            output.value.clone(),
            output_addresses[i].clone(),
            output_types[i].clone(),
        )?;
    }
    
    Ok(())
}