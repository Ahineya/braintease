//! IR Instructions
//! 
//! Defines all instruction types available in the IR.

use rcc_common::{TempId, LabelId, SourceLocation};
use serde::{Deserialize, Serialize};
use std::fmt;
use crate::ir::{Value, IrType, IrBinaryOp, IrUnaryOp};

/// IR representation of an inline assembly operand
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct AsmOperandIR {
    pub constraint: String,     // e.g., "=r", "r", "+r", "m"
    pub value: Value,           // The IR value for this operand
    pub tied_to: Option<usize>, // For "+r" constraints, which input it's tied to
}

/// IR Instruction
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub enum Instruction {
    /// Binary operation: result = op lhs, rhs
    Binary {
        result: TempId,
        op: IrBinaryOp,
        lhs: Value,
        rhs: Value,
        result_type: IrType,
    },
    
    /// Unary operation: result = op operand
    Unary {
        result: TempId,
        op: IrUnaryOp,
        operand: Value,
        result_type: IrType,
    },
    
    /// Load from memory: result = load ptr
    Load {
        result: TempId,
        ptr: Value,
        result_type: IrType,
    },
    
    /// Store to memory: store value, ptr
    Store {
        value: Value,
        ptr: Value,
    },
    
    /// Get element pointer: result = getelementptr ptr, index
    /// Input ptr must be a FatPtr, result will be stored as temp but represents a fat pointer
    /// The backend must handle bank overflow when computing the final address
    /// 
    /// CRITICAL: The `source_element_type` tells us what we're indexing INTO:
    /// - For array[i]: source is array element type, index gets multiplied by its size
    /// - For struct.field: source is struct type, index is the field offset in words
    /// - For ptr + n: source is pointed-to type, index gets multiplied by its size
    GetElementPtr {
        result: TempId,          // Result temp that will hold the computed address
        ptr: Value,              // Must be a FatPtr with bank info
        indices: Vec<Value>,     // Offsets to apply
        result_type: IrType,     // Type of the result pointer
        source_element_type: IrType, // Type of element being indexed (what ptr points to)
        is_struct_field: bool,   // True if this is struct field access (offset in words), false for array/pointer indexing
    },
    
    /// Allocate stack memory: result = alloca type, count
    Alloca {
        result: TempId,
        alloc_type: IrType,
        count: Option<Value>,
        result_type: IrType, // Always pointer type
    },
    
    /// Function call: result = call func(args...)
    Call {
        result: Option<TempId>,
        function: Value,
        args: Vec<Value>,
        result_type: IrType,
        /// If set, arguments at this index and beyond are always passed on the
        /// stack (variadic extras). Named arguments still follow the normal ABI.
        #[serde(default)]
        stack_args_from: Option<usize>,
    },
    
    /// Return: ret value or ret void
    Return(Option<Value>),
    
    /// Unconditional branch: br label
    Branch(LabelId),
    
    /// Conditional branch: br condition, true_label, false_label
    BranchCond {
        condition: Value,
        true_label: LabelId,
        false_label: LabelId,
    },
    
    /// Phi node: result = phi [val1, label1], [val2, label2], ...
    Phi {
        result: TempId,
        incoming: Vec<(Value, LabelId)>,
        result_type: IrType,
    },
    
    /// Type cast: result = cast value to target_type
    Cast {
        result: TempId,
        value: Value,
        target_type: IrType,
    },
    
    /// Select (ternary): result = select condition, true_value, false_value
    Select {
        result: TempId,
        condition: Value,
        true_value: Value,
        false_value: Value,
        result_type: IrType,
    },
    
    /// Intrinsic function call (for compiler builtins)
    Intrinsic {
        result: Option<TempId>,
        intrinsic: String,
        args: Vec<Value>,
        result_type: IrType,
    },
    
    /// Debug information
    DebugLoc {
        location: SourceLocation,
    },
    
    /// Inline assembly
    InlineAsm {
        assembly: String,
        outputs: Vec<AsmOperandIR>,
        inputs: Vec<AsmOperandIR>,
        clobbers: Vec<String>,
    },
    
    /// Comment (for debugging)
    Comment(String),
}

impl fmt::Display for Instruction {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Instruction::Binary { result, op, lhs, rhs, result_type } => {
                write!(f, "%{result} = {op} {result_type} {lhs}, {rhs}")
            }
            Instruction::Unary { result, op, operand, result_type } => {
                write!(f, "%{result} = {op} {result_type} {operand} to {result_type}")
            }
            Instruction::Load { result, ptr, result_type } => {
                write!(f, "%{result} = load {result_type}, {result_type}* {ptr}")
            }
            Instruction::Store { value, ptr } => {
                // For stores, we print: store <value>, <type>* <ptr>
                write!(f, "store {value}, i16* {ptr}")
            }
            Instruction::GetElementPtr { result, ptr, indices, result_type: _, source_element_type, is_struct_field } => {
                write!(f, "%{result} = getelementptr {source_element_type}, {ptr}")?;
                for index in indices {
                    write!(f, ", {index}")?;
                }
                // Bank info is in the ptr if it's a FatPtr
                if let Value::FatPtr(ref fp) = ptr {
                    write!(f, " ; bank={:?}", fp.bank)?;
                }
                if *is_struct_field {
                    write!(f, " ; struct-field")?;
                }
                Ok(())
            }
            Instruction::Alloca { result, alloc_type, count, .. } => {
                write!(f, "%{result} = alloca {alloc_type}")?;
                if let Some(count) = count {
                    write!(f, ", {count}")?;
                }
                Ok(())
            }
            Instruction::Call { result, function, args, .. } => {
                if let Some(result) = result {
                    write!(f, "%{result} = ")?;
                }
                write!(f, "call {function}(")?;
                for (i, arg) in args.iter().enumerate() {
                    if i > 0 { write!(f, ", ")?; }
                    write!(f, "{arg}")?;
                }
                write!(f, ")")
            }
            Instruction::Return(Some(value)) => write!(f, "ret {value}"),
            Instruction::Return(None) => write!(f, "ret void"),
            Instruction::Branch(label) => write!(f, "br label %{label}"),
            Instruction::BranchCond { condition, true_label, false_label } => {
                write!(f, "br {condition} {condition}, label %{true_label}, label %{false_label}")
            }
            Instruction::Phi { result, incoming, result_type } => {
                write!(f, "%{result} = phi {result_type} ")?;
                for (i, (value, label)) in incoming.iter().enumerate() {
                    if i > 0 { write!(f, ", ")?; }
                    write!(f, "[{value}, %{label}]")?;
                }
                Ok(())
            }
            Instruction::Cast { result, value, target_type } => {
                write!(f, "%{result} = cast {value} to {target_type}")
            }
            Instruction::Select { result, condition, true_value, false_value, result_type } => {
                write!(f, "%{result} = select {result_type} {condition}, {result_type} {true_value}, {result_type} {false_value}")
            }
            Instruction::Intrinsic { result, intrinsic, args, .. } => {
                if let Some(result) = result {
                    write!(f, "%{result} = ")?;
                }
                write!(f, "call @{intrinsic}(")?;
                for (i, arg) in args.iter().enumerate() {
                    if i > 0 { write!(f, ", ")?; }
                    write!(f, "{arg}")?;
                }
                write!(f, ")")
            }
            Instruction::DebugLoc { location } => {
                write!(f, "!dbg !{}", location.line)
            }
            Instruction::InlineAsm { assembly, outputs, inputs, clobbers } => {
                write!(f, "asm \"{assembly}\"")?;
                if !outputs.is_empty() || !inputs.is_empty() || !clobbers.is_empty() {
                    write!(f, " : ")?;
                    // Outputs
                    for (i, op) in outputs.iter().enumerate() {
                        if i > 0 { write!(f, ", ")?; }
                        write!(f, "\"{}\"({})", op.constraint, op.value)?;
                    }
                    write!(f, " : ")?;
                    // Inputs
                    for (i, op) in inputs.iter().enumerate() {
                        if i > 0 { write!(f, ", ")?; }
                        write!(f, "\"{}\"({})", op.constraint, op.value)?;
                    }
                    if !clobbers.is_empty() {
                        write!(f, " : ")?;
                        for (i, clob) in clobbers.iter().enumerate() {
                            if i > 0 { write!(f, ", ")?; }
                            write!(f, "\"{clob}\"")?;
                        }
                    }
                }
                Ok(())
            },
            Instruction::Comment(text) => write!(f, "; {text}"),
        }
    }
}