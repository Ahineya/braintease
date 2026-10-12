use rcc_frontend::ir::{Function, Value};
use std::collections::BTreeSet;

pub fn value_symbols(value: &Value, symbols: &mut BTreeSet<String>) {
    match value {
        Value::Global(n) | Value::Function(n) => {
            symbols.insert(n.clone());
        }
        Value::FatPtr(p) => value_symbols(&p.addr, symbols),
        Value::AddressOffset { base, .. } => value_symbols(base, symbols),
        Value::ConstantAggregate(v) => v.iter().for_each(|v| value_symbols(v, symbols)),
        _ => {}
    }
}

pub fn function_symbols(function: &Function) -> BTreeSet<String> {
    let mut symbols = BTreeSet::new();
    for instruction in function.blocks.iter().flat_map(|b| &b.instructions) {
        for value in super::operands(instruction) {
            value_symbols(value, &mut symbols);
        }
    }
    symbols
}

pub fn function_has_opaque_asm(function: &Function) -> bool {
    function.blocks.iter().flat_map(|b| &b.instructions).any(|instruction| {
        matches!(instruction, rcc_frontend::ir::Instruction::InlineAsm { assembly, clobbers, .. }
            if !super::inline::safe_asm(assembly, clobbers))
    })
}
