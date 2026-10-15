//! Printer for the lossless IR text format.

use crate::ir::{
    AsmOperandIR, BasicBlock, Function, GlobalVariable, Instruction, IrType, Linkage, Module, Value,
};
use crate::BankTag;
use std::fmt::Write as _;

pub fn emit_module(module: &Module) -> String {
    let mut out = String::new();
    let _ = write!(out, "module {} {{\n", emit_name(&module.name));

    if module.target != crate::target::Target::Ripple {
        let _ = writeln!(out, "  target {}", module.target.name());
    }
    let mut type_names: Vec<&String> = module.type_definitions.keys().collect();
    type_names.sort();
    for name in type_names {
        let ty = &module.type_definitions[name];
        let _ = writeln!(out, "  type {} = {}", emit_name(name), emit_type(ty));
    }
    for name in &module.internal_functions {
        let _ = writeln!(out, "  internal function {}", emit_at(name));
    }
    for global in &module.globals {
        let _ = writeln!(out, "  {}", emit_global(global));
    }
    for function in &module.functions {
        out.push_str(&emit_function(function));
    }
    out.push_str("}\n");
    out
}

fn emit_function(func: &Function) -> String {
    let mut out = String::from("  function");
    if func.is_external {
        out.push_str(" external");
    }
    if func.is_vararg {
        out.push_str(" vararg");
    }
    let _ = write!(
        out,
        " {} {}({})",
        emit_type(&func.return_type),
        emit_at(&func.name),
        emit_params(&func.parameters)
    );
    if let Some(symbol) = func.symbol_id {
        let _ = write!(out, " symbol {symbol}");
    }
    if let Some(loc) = &func.source_location {
        let _ = write!(
            out,
            " loc {} {} {}",
            emit_name(&loc.filename),
            loc.line,
            loc.column
        );
    }
    out.push_str(" {\n");
    for block in &func.blocks {
        out.push_str(&emit_block(block));
    }
    out.push_str("  }\n");
    out
}

fn emit_params(params: &[(u32, IrType)]) -> String {
    params
        .iter()
        .map(|(id, ty)| format!("{} %{id}", emit_type(ty)))
        .collect::<Vec<_>>()
        .join(", ")
}

fn emit_block(block: &BasicBlock) -> String {
    let mut out = format!("    block {}", block.id);
    if block.is_loop_condition {
        out.push_str(" loop");
    }
    if !block.predecessors.is_empty() {
        let _ = write!(out, " preds {}", join_ids(&block.predecessors));
    }
    if !block.successors.is_empty() {
        let _ = write!(out, " succs {}", join_ids(&block.successors));
    }
    out.push_str(":\n");
    for inst in &block.instructions {
        out.push_str("      ");
        out.push_str(&emit_instruction(inst));
        out.push('\n');
    }
    out
}

fn join_ids(ids: &[u32]) -> String {
    ids.iter()
        .map(|id| id.to_string())
        .collect::<Vec<_>>()
        .join(", ")
}

fn emit_global(global: &GlobalVariable) -> String {
    let mut out = format!(
        "{} = {} ",
        emit_at(&global.name),
        emit_linkage(global.linkage)
    );
    if global.is_constant {
        out.push_str("constant ");
    }
    out.push_str(&emit_type(&global.var_type));
    if let Some(init) = &global.initializer {
        out.push_str(" = ");
        out.push_str(&emit_value(init));
    }
    if let Some(symbol) = global.symbol_id {
        let _ = write!(out, " symbol {symbol}");
    }
    out
}

fn emit_linkage(linkage: Linkage) -> &'static str {
    match linkage {
        Linkage::External => "external",
        Linkage::Internal => "internal",
        Linkage::Private => "private",
        Linkage::Declaration => "declaration",
    }
}

fn emit_instruction(inst: &Instruction) -> String {
    match inst {
        Instruction::Binary { result, op, lhs, rhs, result_type } => {
            format!(
                "%{result} = {op} {} {}, {}",
                emit_type(result_type),
                emit_value(lhs),
                emit_value(rhs)
            )
        }
        Instruction::Unary { result, op, operand, result_type } => {
            format!("%{result} = {op} {} {}", emit_type(result_type), emit_value(operand))
        }
        Instruction::Load { result, ptr, result_type, volatile } => {
            format!(
                "%{result} = load {}{}, {}",
                if *volatile { "volatile " } else { "" },
                emit_type(result_type),
                emit_value(ptr)
            )
        }
        Instruction::Store { value, ptr, value_type, volatile } => {
            format!(
                "store {}{} {}, {}",
                if *volatile { "volatile " } else { "" },
                emit_type(value_type),
                emit_value(value),
                emit_value(ptr)
            )
        }
        Instruction::GetElementPtr {
            result,
            ptr,
            indices,
            result_type,
            source_element_type,
            is_struct_field,
        } => {
            let mut out = format!(
                "%{result} = getelementptr {}, {} {}",
                emit_type(result_type),
                emit_type(source_element_type),
                emit_value(ptr)
            );
            for index in indices {
                out.push_str(", ");
                out.push_str(&emit_value(index));
            }
            if *is_struct_field {
                out.push_str(" field");
            }
            out
        }
        Instruction::Alloca { result, alloc_type, count, result_type } => {
            let mut out = format!("%{result} = alloca {}", emit_type(alloc_type));
            if let Some(count) = count {
                out.push_str(", ");
                out.push_str(&emit_value(count));
            }
            out.push_str(" -> ");
            out.push_str(&emit_type(result_type));
            out
        }
        Instruction::Call { result, function, args, result_type, stack_args_from } => {
            let mut out = String::new();
            if let Some(result) = result {
                let _ = write!(out, "%{result} = ");
            }
            let _ = write!(
                out,
                "call {} {}({})",
                emit_type(result_type),
                emit_value(function),
                join_values(args)
            );
            if let Some(from) = stack_args_from {
                let _ = write!(out, " stack {from}");
            }
            out
        }
        Instruction::Return(Some(value)) => format!("ret {}", emit_value(value)),
        Instruction::Return(None) => "ret void".to_string(),
        Instruction::Branch(label) => format!("br label {label}"),
        Instruction::BranchCond { condition, true_label, false_label } => {
            format!(
                "br {}, label {true_label}, label {false_label}",
                emit_value(condition)
            )
        }
        Instruction::Phi { result, incoming, result_type } => {
            let incoming = incoming
                .iter()
                .map(|(value, label)| format!("[{}, label {label}]", emit_value(value)))
                .collect::<Vec<_>>()
                .join(", ");
            if incoming.is_empty() {
                format!("%{result} = phi {}", emit_type(result_type))
            } else {
                format!("%{result} = phi {} {incoming}", emit_type(result_type))
            }
        }
        Instruction::Cast { result, value, target_type } => {
            format!("%{result} = cast {} to {}", emit_value(value), emit_type(target_type))
        }
        Instruction::Select { result, condition, true_value, false_value, result_type } => {
            format!(
                "%{result} = select {} {}, {}, {}",
                emit_type(result_type),
                emit_value(condition),
                emit_value(true_value),
                emit_value(false_value)
            )
        }
        Instruction::Intrinsic { result, intrinsic, args, result_type } => {
            let mut out = String::new();
            if let Some(result) = result {
                let _ = write!(out, "%{result} = ");
            }
            let _ = write!(
                out,
                "intrinsic {} {}({})",
                emit_type(result_type),
                emit_at(intrinsic),
                join_values(args)
            );
            out
        }
        Instruction::DebugLoc { location } => {
            format!(
                "debugloc {} {} {}",
                emit_name(&location.filename),
                location.line,
                location.column
            )
        }
        Instruction::InlineAsm { assembly, outputs, inputs, clobbers } => {
            let mut out = format!("asm {}", emit_string(assembly));
            if !outputs.is_empty() {
                out.push_str(" outs ");
                out.push_str(&join_operands(outputs));
            }
            if !inputs.is_empty() {
                out.push_str(" ins ");
                out.push_str(&join_operands(inputs));
            }
            if !clobbers.is_empty() {
                out.push_str(" clobbers ");
                out.push_str(
                    &clobbers
                        .iter()
                        .map(|clobber| emit_string(clobber))
                        .collect::<Vec<_>>()
                        .join(", "),
                );
            }
            out
        }
        Instruction::Comment(text) => format!("comment {}", emit_string(text)),
    }
}

fn join_values(values: &[Value]) -> String {
    values
        .iter()
        .map(emit_value)
        .collect::<Vec<_>>()
        .join(", ")
}

fn join_operands(operands: &[AsmOperandIR]) -> String {
    operands
        .iter()
        .map(emit_operand)
        .collect::<Vec<_>>()
        .join(", ")
}

fn emit_operand(op: &AsmOperandIR) -> String {
    let mut out = format!("{}({})", emit_string(&op.constraint), emit_value(&op.value));
    if let Some(tied) = op.tied_to {
        let _ = write!(out, " tie {tied}");
    }
    out
}

pub(super) fn emit_type(ty: &IrType) -> String {
    match ty {
        IrType::Void => "void".to_string(),
        IrType::I1 => "i1".to_string(),
        IrType::I8 => "i8".to_string(),
        IrType::I16 => "i16".to_string(),
        IrType::I32 => "i32".to_string(),
        IrType::I64 => "i64".to_string(),
        IrType::F32 => "f32".to_string(),
        IrType::F64 => "f64".to_string(),
        IrType::Label => "label".to_string(),
        IrType::FatPtr(inner) => format!("{}*", emit_type(inner)),
        IrType::Array { size, element_type } => {
            format!("[{size} x {}]", emit_type(element_type))
        }
        IrType::Function { return_type, param_types, is_vararg } => {
            let mut params: Vec<String> = param_types.iter().map(emit_type).collect();
            if *is_vararg {
                params.push("...".to_string());
            }
            format!("{}({})", emit_type(return_type), params.join(", "))
        }
        IrType::Struct { name, fields, packed } => {
            let mut out = String::new();
            if *packed {
                out.push_str("packed ");
            }
            out.push_str("struct");
            if let Some(name) = name {
                out.push(' ');
                out.push_str(&emit_name(name));
            }
            out.push_str(" { ");
            out.push_str(
                &fields
                    .iter()
                    .map(emit_type)
                    .collect::<Vec<_>>()
                    .join(", "),
            );
            if !fields.is_empty() {
                out.push(' ');
            }
            out.push('}');
            out
        }
    }
}

pub(super) fn emit_value(value: &Value) -> String {
    match value {
        Value::Temp(id) => format!("%{id}"),
        Value::Constant(n) => n.to_string(),
        Value::Global(name) => format!("global {}", emit_at(name)),
        Value::Function(name) => format!("func {}", emit_at(name)),
        Value::Undef => "undef".to_string(),
        Value::AddressOffset { base, cells } => {
            format!("offset({}, {cells})", emit_value(base))
        }
        Value::FatPtr(ptr) => {
            format!("fat({}, {})", emit_value(&ptr.addr), emit_bank(ptr.bank))
        }
        Value::ConstantArray(values) => {
            let body = values
                .iter()
                .map(|value| value.to_string())
                .collect::<Vec<_>>()
                .join(", ");
            format!("[{body}]")
        }
        Value::ConstantAggregate(values) => format!("{{{}}}", join_values(values)),
    }
}

fn emit_bank(bank: BankTag) -> String {
    match bank {
        BankTag::Global => "global".to_string(),
        BankTag::Stack => "stack".to_string(),
        BankTag::Heap(n) => format!("heap({n})"),
        BankTag::Unknown => "unknown".to_string(),
        BankTag::Mixed => "mixed".to_string(),
        BankTag::Null => "null".to_string(),
    }
}

fn emit_at(name: &str) -> String {
    format!("@{}", emit_name(name))
}

pub(super) fn emit_name(name: &str) -> String {
    if is_bare_name(name) && !is_keyword(name) {
        name.to_string()
    } else {
        emit_string(name)
    }
}

fn is_bare_name(name: &str) -> bool {
    let mut chars = name.chars();
    match chars.next() {
        Some(c) if c.is_ascii_alphabetic() || c == '_' || c == '$' => {}
        _ => return false,
    }
    chars.all(|c| c.is_ascii_alphanumeric() || c == '_' || c == '.' || c == '$')
}

pub(super) fn emit_string(text: &str) -> String {
    let mut out = String::from("\"");
    for ch in text.chars() {
        match ch {
            '\\' => out.push_str("\\\\"),
            '"' => out.push_str("\\\""),
            '\n' => out.push_str("\\n"),
            '\r' => out.push_str("\\r"),
            '\t' => out.push_str("\\t"),
            ch if ch.is_control() => {
                let _ = write!(out, "\\u{{{:x}}}", ch as u32);
            }
            ch => out.push(ch),
        }
    }
    out.push('"');
    out
}

fn is_keyword(name: &str) -> bool {
    matches!(
        name,
        "module"
            | "type"
            | "internal"
            | "function"
            | "external"
            | "private"
            | "declaration"
            | "constant"
            | "symbol"
            | "loc"
            | "vararg"
            | "block"
            | "loop"
            | "preds"
            | "succs"
            | "void"
            | "i1"
            | "i8"
            | "i16"
            | "i32"
            | "i64"
            | "f32"
            | "f64"
            | "label"
            | "struct"
            | "packed"
            | "global"
            | "func"
            | "fat"
            | "offset"
            | "undef"
            | "heap"
            | "stack"
            | "unknown"
            | "mixed"
            | "null"
            | "add"
            | "sub"
            | "mul"
            | "sdiv"
            | "udiv"
            | "srem"
            | "urem"
            | "and"
            | "or"
            | "xor"
            | "shl"
            | "lshr"
            | "ashr"
            | "eq"
            | "ne"
            | "slt"
            | "sle"
            | "sgt"
            | "sge"
            | "ult"
            | "ule"
            | "ugt"
            | "uge"
            | "not"
            | "neg"
            | "zext"
            | "sext"
            | "trunc"
            | "ptrtolinear"
            | "ptrtoint"
            | "inttoptr"
            | "load"
            | "store"
            | "volatile"
            | "getelementptr"
            | "field"
            | "alloca"
            | "call"
            | "ret"
            | "br"
            | "phi"
            | "cast"
            | "select"
            | "intrinsic"
            | "debugloc"
            | "asm"
            | "outs"
            | "ins"
            | "clobbers"
            | "tie"
            | "comment"
            | "to"
            | "x"
    )
}
