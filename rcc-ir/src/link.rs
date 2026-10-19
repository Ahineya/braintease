//! Target-neutral whole-program IR linking. Private symbols are
//! scoped per translation unit; definitions resolve declarations before DCE.
use crate::{ir::*, target::Target};
use std::collections::{BTreeMap, BTreeSet, HashSet};

pub fn link_modules(modules: &[Module], target: Target, roots: &[&str]) -> Result<Module, String> {
    let mut out = Module::new(format!("{}-program", target.name()));
    out.target = target;
    let mut globals = BTreeMap::<String, GlobalVariable>::new();
    let mut functions = BTreeMap::<String, Function>::new();
    for (index, input) in modules.iter().enumerate() {
        if input.target != target {
            return Err(format!(
                "module {} was not compiled for {}",
                input.name,
                target.name()
            ));
        }
        let mut unit = input.clone();
        let private: BTreeMap<_, _> = unit
            .globals
            .iter()
            .filter(|g| matches!(g.linkage, Linkage::Internal | Linkage::Private))
            .map(|g| g.name.clone())
            .chain(unit.internal_functions.iter().cloned())
            .map(|n| (n.clone(), format!("__unit{index}_{n}")))
            .collect();
        let rename = |v: &mut Value| rename_value(v, &private);
        for g in &mut unit.globals {
            if let Some(n) = private.get(&g.name) {
                g.name = n.clone();
            }
            if let Some(v) = &mut g.initializer {
                rename(v);
            }
        }
        for f in &mut unit.functions {
            if let Some(n) = private.get(&f.name) {
                f.name = n.clone();
            }
            for i in f.blocks.iter_mut().flat_map(|b| &mut b.instructions) {
                visit(i, &rename);
            }
        }
        out.internal_functions
            .extend(unit.internal_functions.iter().map(|n| private[n].clone()));
        for g in unit.globals {
            if let Some(old) = globals.get_mut(&g.name) {
                if old.linkage == Linkage::Declaration {
                    if g.linkage != Linkage::Declaration {
                        *old = g;
                    }
                } else if g.linkage != Linkage::Declaration {
                    if old.initializer.is_some() && g.initializer.is_some() {
                        return Err(format!("multiple definitions of global {}", g.name));
                    }
                    if old.var_type != g.var_type {
                        return Err(format!("conflicting global layouts for {}", g.name));
                    }
                    if g.initializer.is_some() {
                        *old = g;
                    }
                }
            } else {
                globals.insert(g.name.clone(), g);
            }
        }
        for f in unit.functions {
            if let Some(old) = functions.get_mut(&f.name) {
                if old.parameters.len() != f.parameters.len()
                    || old.is_vararg != f.is_vararg
                    || !same_carrier(&old.return_type, &f.return_type)
                    || !old
                        .parameters
                        .iter()
                        .zip(&f.parameters)
                        .all(|((_, a), (_, b))| same_carrier(a, b))
                {
                    return Err(format!(
                        "conflicting {} signatures for function {}",
                        target.name(),
                        f.name
                    ));
                }
                if old.is_external {
                    if !f.is_external {
                        *old = f;
                    }
                } else if !f.is_external {
                    return Err(format!("multiple definitions of function {}", f.name));
                }
            } else {
                functions.insert(f.name.clone(), f);
            }
        }
    }
    out.globals = globals.into_values().collect();
    out.functions = functions.into_values().collect();
    if let Some(g) = out.globals.iter().find(|g| functions_named(&out, &g.name)) {
        return Err(format!(
            "{} symbol {} is both a function and a global",
            target.name(),
            g.name
        ));
    }
    retain_reachable(&mut out, roots);
    Ok(out)
}

fn functions_named(module: &Module, name: &str) -> bool {
    module.functions.iter().any(|f| f.name == name)
}

// Pointer pointees can be incomplete in a declaration. They have the same
// scalar carrier; numeric widths and variadic packing must agree across units.
fn same_carrier(a: &IrType, b: &IrType) -> bool {
    matches!((a, b), (IrType::FatPtr(_), IrType::FatPtr(_))) || a == b
}

pub fn retain_reachable(module: &mut Module, roots: &[&str]) {
    let mut pending: Vec<_> = roots.iter().map(|name| (*name).to_string()).collect();
    let mut seen = HashSet::new();
    let funcs: BTreeMap<_, _> = module.functions.iter().map(|f| (&f.name, f)).collect();
    let globals: BTreeMap<_, _> = module.globals.iter().map(|g| (&g.name, g)).collect();
    while let Some(name) = pending.pop() {
        if !seen.insert(name.clone()) {
            continue;
        }
        let mut refs = BTreeSet::new();
        if let Some(f) = funcs.get(&name) {
            refs.extend(function_symbols(f));
        }
        if let Some(g) = globals.get(&name) {
            if let Some(v) = &g.initializer {
                value_symbols(v, &mut refs);
            }
        }
        pending.extend(refs);
    }
    module.functions.retain(|f| seen.contains(&f.name));
    module.globals.retain(|g| seen.contains(&g.name));
    let functions: BTreeSet<_> = module.functions.iter().map(|f| f.name.clone()).collect();
    module.internal_functions.retain(|n| functions.contains(n));
}

fn value_symbols(value: &Value, symbols: &mut BTreeSet<String>) {
    match value {
        Value::Global(n) | Value::Function(n) => {
            symbols.insert(n.clone());
        }
        Value::FatPtr(p) => value_symbols(&p.addr, symbols),
        Value::AddressOffset { base, .. } => value_symbols(base, symbols),
        Value::ConstantAggregate(values) => {
            for v in values {
                value_symbols(v, symbols);
            }
        }
        _ => {}
    }
}

pub fn referenced_symbols(module: &Module) -> BTreeSet<String> {
    let mut symbols = BTreeSet::new();
    for function in &module.functions { symbols.extend(function_symbols(function)); }
    for global in &module.globals {
        if let Some(value) = &global.initializer { value_symbols(value, &mut symbols); }
    }
    symbols
}
pub fn function_symbols(function: &Function) -> BTreeSet<String> {
    let mut symbols = BTreeSet::new();
    for instruction in function.blocks.iter().flat_map(|b| &b.instructions) {
        let values: Vec<&Value> = match instruction {
            Instruction::Binary { lhs, rhs, .. } => vec![lhs, rhs],
            Instruction::Unary { operand, .. } => vec![operand],
            Instruction::Load { ptr, .. } => vec![ptr],
            Instruction::Store { value, ptr, .. } => vec![value, ptr],
            Instruction::GetElementPtr { ptr, indices, .. } => {
                std::iter::once(ptr).chain(indices).collect()
            }
            Instruction::Alloca { count, .. } | Instruction::Return(count) => {
                count.iter().collect()
            }
            Instruction::Call { function, args, .. } => {
                std::iter::once(function).chain(args).collect()
            }
            Instruction::Intrinsic { args, .. } => args.iter().collect(),
            Instruction::BranchCond { condition, .. } => vec![condition],
            Instruction::Phi { incoming, .. } => incoming.iter().map(|(v, _)| v).collect(),
            Instruction::Cast { value, .. } => vec![value],
            Instruction::Select {
                condition,
                true_value,
                false_value,
                ..
            } => vec![condition, true_value, false_value],
            Instruction::InlineAsm {
                inputs, outputs, ..
            } => inputs.iter().chain(outputs).map(|o| &o.value).collect(),
            Instruction::Branch(_) | Instruction::DebugLoc { .. } | Instruction::Comment(_) => {
                Vec::new()
            }
        };
        for value in values {
            value_symbols(value, &mut symbols);
        }
    }
    symbols
}

fn rename_value(value: &mut Value, names: &BTreeMap<String, String>) {
    match value {
        Value::Global(n) | Value::Function(n) => {
            if let Some(new) = names.get(n) {
                *n = new.clone();
            }
        }
        Value::FatPtr(p) => rename_value(&mut p.addr, names),
        Value::AddressOffset { base, .. } => rename_value(base, names),
        Value::ConstantAggregate(values) => {
            for v in values {
                rename_value(v, names)
            }
        }
        _ => {}
    }
}
fn visit(i: &mut Instruction, f: &impl Fn(&mut Value)) {
    use Instruction::*;
    match i {
        Binary { lhs, rhs, .. } => {
            f(lhs);
            f(rhs);
        }
        Unary { operand, .. } => f(operand),
        Load { ptr, .. } => f(ptr),
        Store { value, ptr, .. } => {
            f(value);
            f(ptr);
        }
        GetElementPtr { ptr, indices, .. } => {
            f(ptr);
            for v in indices {
                f(v);
            }
        }
        Alloca { count, .. } | Return(count) => {
            if let Some(v) = count {
                f(v);
            }
        }
        Call { function, args, .. } => {
            f(function);
            for v in args {
                f(v);
            }
        }
        Intrinsic { args, .. } => {
            for v in args {
                f(v);
            }
        }
        BranchCond { condition, .. } => f(condition),
        Phi { incoming, .. } => {
            for (v, _) in incoming {
                f(v);
            }
        }
        Cast { value, .. } => f(value),
        Select {
            condition,
            true_value,
            false_value,
            ..
        } => {
            f(condition);
            f(true_value);
            f(false_value);
        }
        InlineAsm {
            inputs, outputs, ..
        } => {
            for operand in inputs.iter_mut().chain(outputs) {
                f(&mut operand.value);
            }
        }
        Branch(_) | DebugLoc { .. } | Comment(_) => {}
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn declarations_must_match_the_native_definition_signature() {
        let mut declaration = Function::new("value".into(), IrType::I16);
        declaration.is_external = true;
        declaration.add_parameter(0, IrType::I32);
        let mut definition = declaration.clone();
        definition.is_external = false;
        definition.parameters[0].1 = IrType::I16;
        let mut a = Module::new("decl".into());
        a.target = Target::Native;
        a.add_function(declaration);
        let mut b = Module::new("def".into());
        b.target = Target::Native;
        b.add_function(definition);
        assert!(link_modules(&[a, b], Target::Native, &["main"])
            .unwrap_err()
            .contains("conflicting native signatures"));
    }
}

/// Compatibility backends reserves constant local storage once per call.
/// A C goto may skip a declaration's initializer and still use its storage.
/// Keep those addresses in the entry block before unreachable-block removal.
pub fn hoist_static_allocas(module: &mut Module) {
    for function in &mut module.functions {
        let mut allocations = Vec::new();
        for block in &mut function.blocks {
            block.instructions.retain(|instruction| {
                if matches!(
                    instruction,
                    Instruction::Alloca {
                        count: None | Some(Value::Constant(_)),
                        ..
                    }
                ) {
                    allocations.push(instruction.clone());
                    false
                } else {
                    true
                }
            });
        }
        if let Some(entry) = function.blocks.first_mut() {
            allocations.append(&mut entry.instructions);
            entry.instructions = allocations;
        }
    }
}
