//! Public browser entry points use i32 values, never private i64 carriers.
use rcc_ir::ir::{Function as IrFunction, IrType};
use std::collections::BTreeMap;
use wasm_encoder::{
    CodeSection, ExportKind, ExportSection, Function, FunctionSection, Instruction as W,
    TypeSection, ValType,
};

pub(crate) const ROOTS: [&str; 4] = [
    "__wasm_browser_inbox",
    "__wasm_browser_events",
    "__wasm_browser_dispose",
    "__wasm_heap_metrics",
];
pub(crate) const EFFECT_ROOTS: [&str; 4] = [
    "__wasm_effect_inbox",
    "__wasm_effect_deliver",
    "__wasm_effect_flush",
    "__wasm_effect_shutdown",
];
pub(crate) const EXECUTOR_ROOTS: [&str; 2] = ["__wasm_browser_clock", "__wasm_browser_turn"];
pub(crate) const EVENT_CELLS: i32 = 4096;
pub(crate) fn types(types: &mut TypeSection) -> [u32; 5] {
    let first = types.len();
    types.ty().function([], [ValType::I32]);
    types.ty().function([ValType::I32], []);
    types.ty().function([], []);
    types.ty().function([ValType::I32], [ValType::I32]);
    types.ty().function([ValType::I32, ValType::I32], []);
    [first, first + 1, first + 2, first + 3, first + 4]
}
pub(crate) fn emit(
    indices: &BTreeMap<String, (u32, &IrFunction)>,
    address: u32,
    effects: bool,
    executor: bool,
    types: &[u32; 5],
    functions: &mut FunctionSection,
    exports: &mut ExportSection,
    code: &mut CodeSection,
    first: u32,
) -> Result<(), String> {
    let get = |name: &str, params: &[IrType], result: &IrType| -> Result<u32, String> {
        let (index, f) = indices
            .get(name)
            .ok_or_else(|| format!("browser application requires {name}"))?;
        if f.is_external
            || f.is_vararg
            || f.return_type != *result
            || f.parameters.iter().map(|(_, ty)| ty).ne(params.iter())
        {
            return Err(format!("invalid browser entry signature {name}"));
        }
        Ok(*index)
    };
    let ptr = IrType::FatPtr(Box::new(IrType::I16));
    // Hook arguments are borrowed only for the event batch. C16 unsigned short
    // and RFL u16 have the same representation (pointer pointees are checked).
    get(
        "ripple_browser_on_events",
        &[ptr.clone(), IrType::I32],
        &IrType::Void,
    )?;
    get("ripple_browser_on_dispose", &[], &IrType::Void)?;
    let (_, main) = indices
        .get("main")
        .ok_or("browser applications require main")?;
    if !main.parameters.is_empty()
        || !matches!(main.return_type, IrType::Void | IrType::I16 | IrType::I32)
    {
        return Err("browser main must take no arguments and return void, i16 or i32".into());
    }
    let start = indices["main"].0;
    let inbox = get(ROOTS[0], &[], &ptr)?;
    let events = get(ROOTS[1], &[IrType::I32], &IrType::Void)?;
    let dispose = get(ROOTS[2], &[], &IrType::Void)?;
    let metrics = get(ROOTS[3], &[], &ptr)?;
    let mut wrappers = vec![
        (
            "__ripple_start",
            types[0],
            vec![W::Call(start), W::I32WrapI64],
        ),
        (
            "__ripple_event_buffer",
            types[0],
            vec![
                W::Call(inbox),
                W::I64Const(EVENT_CELLS.into()),
                W::Call(address),
                W::I32WrapI64,
            ],
        ),
        (
            "__ripple_event_capacity",
            types[0],
            vec![W::I32Const(EVENT_CELLS)],
        ),
        (
            "__ripple_events",
            types[1],
            vec![W::LocalGet(0), W::I64ExtendI32U, W::Call(events), W::Drop],
        ),
        (
            "__ripple_dispose",
            types[2],
            vec![W::Call(dispose), W::Drop],
        ),
        ("__ripple_browser_abi", types[0], vec![W::I32Const(1)]),
        (
            "__ripple_heap_metrics",
            types[0],
            vec![
                W::Call(metrics),
                W::I64Const(6),
                W::Call(address),
                W::I32WrapI64,
            ],
        ),
    ];
    if effects {
        get("ripple_browser_on_effects", &[], &IrType::Void)?;
        let inbox = get(EFFECT_ROOTS[0], &[], &ptr)?;
        let deliver = get(EFFECT_ROOTS[1], &[IrType::I32], &IrType::Void)?;
        get(EFFECT_ROOTS[2], &[], &IrType::Void)?;
        get(EFFECT_ROOTS[3], &[], &IrType::Void)?;
        // Flush host effects only after all guest borrows have ended. Disposing
        // cancels guest work first, then frees undelivered completion payloads.
        wrappers.push((
            "__ripple_effect_buffer",
            types[0],
            vec![
                W::Call(inbox),
                W::I64Const(32768),
                W::Call(address),
                W::I32WrapI64,
            ],
        ));
        wrappers.push((
            "__ripple_completions",
            types[1],
            vec![W::LocalGet(0), W::I64ExtendI32U, W::Call(deliver), W::Drop],
        ));
    }
    if executor {
        get("ripple_browser_on_turn", &[], &IrType::I32)?;
        let clock = get(EXECUTOR_ROOTS[0], &[IrType::I32], &IrType::Void)?;
        get(EXECUTOR_ROOTS[1], &[], &IrType::Void)?;
        wrappers.push(("__ripple_turn", types[1], vec![]));
        for (name, ty, body) in &mut wrappers {
            let argument = match *name {
                "__ripple_start" => {
                    *ty = types[3];
                    Some(0)
                }
                "__ripple_events" | "__ripple_completions" => {
                    *ty = types[4];
                    Some(1)
                }
                "__ripple_turn" => Some(0),
                _ => None,
            };
            if let Some(argument) = argument {
                body.splice(
                    0..0,
                    [
                        W::LocalGet(argument),
                        W::I64ExtendI32U,
                        W::Call(clock),
                        W::Drop,
                    ],
                );
            }
        }
    }
    for (name, _, body) in &mut wrappers {
        let active = matches!(
            *name,
            "__ripple_start" | "__ripple_events" | "__ripple_completions" | "__ripple_turn"
        );
        if active && executor {
            body.extend([W::Call(indices[EXECUTOR_ROOTS[1]].0), W::Drop]);
        }
        if effects {
            if *name == "__ripple_dispose" {
                body.extend([W::Call(indices[EFFECT_ROOTS[3]].0), W::Drop]);
            } else if active {
                body.extend([W::Call(indices[EFFECT_ROOTS[2]].0), W::Drop]);
            }
        }
    }
    // Finish output inside the same guest turn. The host should not need a
    // second Wasm entry merely to flush a partial line after each event batch.
    if let Some((flush, _)) = indices.get("__wasm_flush") {
        for (name, _, body) in &mut wrappers {
            if matches!(
                *name,
                "__ripple_start"
                    | "__ripple_events"
                    | "__ripple_completions"
                    | "__ripple_dispose"
                    | "__ripple_turn"
            ) {
                body.extend([W::Call(*flush), W::Drop]);
            }
        }
    }
    for (i, (name, ty, instructions)) in wrappers.into_iter().enumerate() {
        functions.function(ty);
        exports.export(name, ExportKind::Func, first + i as u32);
        let mut body = Function::new([]);
        for instruction in instructions {
            body.instruction(&instruction);
        }
        body.instruction(&W::End);
        code.function(&body);
    }
    Ok(())
}
