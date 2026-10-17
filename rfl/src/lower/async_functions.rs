//! Compiler-owned future frames. Their allocations are independent of the
//! synchronous LIFO activation arena, so handles can move and retire in any order.
use super::*;
use std::borrow::Cow;

const ASYNC_FRAME: i64 = 4;
const ASYNC_COMPLETE: i64 = u16::MAX as i64;

fn async_fail(builder: &mut IrBuilder, code: i64) -> Result<(), CompileError> {
    fail(builder, code)?;
    let result = if builder.current_return_type() == Some(&IrType::Void) {
        None
    } else {
        Some(null_ptr())
    };
    builder.build_return(result).ir()
}

pub(super) fn expand_async_functions<'a>(
    functions: &'a [TFn],
    resources: &crate::resources::ResourceTypes,
) -> Vec<Cow<'a, TFn>> {
    let mut result = Vec::new();
    for function in functions {
        let Some(future_ty) = function.async_result else {
            result.push(Cow::Borrowed(function));
            continue;
        };
        let mut body = function.clone();
        body.name = format!("{}$async_body", function.name);
        let bindings: Vec<_> = function
            .params
            .iter()
            .map(|id| function.bindings[*id as usize])
            .collect();
        let args = bindings
            .iter()
            .enumerate()
            .map(|(id, ty)| TExpr {
                ty: *ty,
                kind: if resources.contains(*ty) {
                    TExprKind::Move {
                        binding: id as u32,
                        span: 0,
                    }
                } else {
                    TExprKind::Name(id as u32, 0)
                },
            })
            .collect();
        result.push(Cow::Owned(TFn {
            async_context: None,
            name: function.name.clone(),
            async_result: None,
            params: (0..bindings.len() as u32).collect(),
            ret: future_ty,
            shared: vec![false; bindings.len()],
            bindings,
            body: TBlock {
                stmts: vec![],
                tail: Some(TExpr {
                    ty: future_ty,
                    kind: TExprKind::AsyncNew {
                        body: body.name.clone(),
                        args,
                    },
                }),
            },
            closure: true,
            captures: vec![],
            c_export: false,
        }));
        result.push(Cow::Owned(body));
    }
    result
}

fn allocate(builder: &mut IrBuilder, cells: i64) -> Result<Value, CompileError> {
    let size = i32_const(builder, cells)?;
    let raw = builder
        .build_call(Value::Global("malloc".into()), vec![size], ptr_ty())
        .ir()?
        .unwrap();
    let ptr = fat_temp(raw, BankTag::Mixed);
    let bad = builder.new_label();
    let ok = builder.new_label();
    let missing = ptr_is_null(builder, &ptr)?;
    builder.build_branch_cond(missing, bad, ok).ir()?;
    builder.create_block(bad).ir()?;
    async_fail(builder, FAIL_ALLOC)?;
    builder.create_block(ok).ir()?;
    Ok(ptr)
}

pub(super) fn emit_async_new(
    builder: &mut IrBuilder,
    proc: &Proc,
    built: &Built,
    frame: &Value,
    insn: &Insn,
) -> Result<(), CompileError> {
    let Insn::AsyncNew { dest, body, args } = insn else {
        unreachable!()
    };
    let dest = *dest;
    let body = built.procs.iter().find(|p| p.name == *body).unwrap();
    let tag = built.collections.owners.len() + body.func_id;
    if tag > u16::MAX as usize || body.frame_cells + ASYNC_FRAME > u16::MAX as i64 {
        return Err(CompileError::plain(
            "compiler-owned future frame or destructor table is too large",
        ));
    }
    let owner = allocate(builder, body.frame_cells + ASYNC_FRAME)?;
    let child = gep(builder, &owner, ASYNC_FRAME)?;
    store_cell(builder, &owner, 0, Value::Constant(1))?;
    store_cell(builder, &owner, 1, Value::Constant(tag as i64))?;
    let child_slot = gep(builder, &owner, 2)?;
    builder
        .build_store(child.clone(), child_slot, ptr_ty())
        .ir()?;
    store_cell(builder, &child, RESUME, Value::Constant(0))?;
    store_cell(
        builder,
        &child,
        FUNC_ID,
        Value::Constant(body.func_id as i64),
    )?;
    let caller = gep(builder, &child, CALLER)?;
    builder.build_store(null_ptr(), caller, ptr_ty()).ir()?;
    store_cell(builder, &child, DEST, Value::Constant(0))?;
    zero_managed_slots(builder, body, &child)?;
    for (arg, param) in args.iter().zip(&body.param_slots) {
        if proc.resources.contains(proc.slots[*arg].ty) {
            transfer_slot(builder, proc, frame, *arg, body, &child, *param)?;
        } else {
            copy_slot(builder, proc, frame, *arg, body, &child, *param)?;
        }
    }
    let object = allocate(builder, OBJ_FIELDS + 2)?;
    store_cell(builder, &object, OBJ_REFS, Value::Constant(1))?;
    store_cell(builder, &object, OBJ_TAG, Value::Constant(0))?;
    let owner_slot = gep(builder, &object, OBJ_FIELDS)?;
    builder.build_store(owner, owner_slot, ptr_ty()).ir()?;
    release_managed(builder, proc, frame, dest)?;
    let offset = proc.slots[dest].offset;
    store_cell(builder, frame, offset, Value::Constant(0))?;
    let pointer = gep(builder, frame, offset + 1)?;
    builder.build_store(object, pointer, ptr_ty()).ir()
}

/// Resolve the private owner field through &mut Async<T>, retaining the outer
/// object's unique identity. Safe code cannot manufacture or copy the payload.
fn future_owner_slot(
    builder: &mut IrBuilder,
    proc: &Proc,
    frame: &Value,
    future: usize,
) -> Result<Value, CompileError> {
    let place = load_fat(builder, frame, proc.slots[future].offset)?;
    let object = load_fat(builder, &place, 1)?;
    gep(builder, &object, OBJ_FIELDS)
}

pub(super) fn emit_async_cancel(
    builder: &mut IrBuilder,
    proc: &Proc,
    frame: &Value,
    future: usize,
) -> Result<(), CompileError> {
    let owner = future_owner_slot(builder, proc, frame, future)?;
    release_managed_through(builder, proc, &owner, Ty::ErasedOwner)
}

/// Entering a future only prepares its continuation; the caller returns this
/// frame to the existing driver. It never recursively runs another driver.
pub(super) fn emit_async_enter(
    builder: &mut IrBuilder,
    proc: &Proc,
    frame: &Value,
    dest: usize,
    future: usize,
    context: usize,
) -> Result<Value, CompileError> {
    let owner_slot = future_owner_slot(builder, proc, frame, future)?;
    let owner = load_fat(builder, &owner_slot, 0)?;
    let context = load_fat(builder, frame, proc.slots[context].offset)?;
    release_managed(builder, proc, frame, dest)?;
    let Ty::Adt(poll) = proc.slots[dest].ty else {
        unreachable!()
    };
    let raw = builder
        .build_call(
            Value::Global(format!("rfl_enter_async_{poll}")),
            vec![
                owner,
                context,
                frame.clone(),
                Value::Constant(proc.slots[dest].offset),
            ],
            ptr_ty(),
        )
        .ir()?
        .unwrap();
    Ok(fat_temp(raw, BankTag::Mixed))
}

pub(super) fn emit_async_runtime(
    builder: &mut IrBuilder,
    module: &mut Module,
    built: &Built,
) -> Result<(), CompileError> {
    let polls: std::collections::BTreeSet<_> = built
        .procs
        .iter()
        .flat_map(|proc| {
            proc.segs.iter().filter_map(|seg| match &seg.term {
                Some(Term::AsyncPoll { dest, .. }) => match proc.slots[*dest].ty {
                    Ty::Adt(id) => Some(id),
                    _ => unreachable!(),
                },
                _ => None,
            })
        })
        .collect();
    for poll in polls {
        let output = built.adts[poll as usize].args[0];
        builder.create_function(format!("rfl_enter_async_{poll}"), ptr_ty());
        builder.add_parameter(0, ptr_ty()); // erased owner
        builder.add_parameter(1, ptr_ty()); // current Context borrow
        builder.add_parameter(2, ptr_ty()); // caller frame
        builder.add_parameter(3, IrType::I16); // caller's Poll<T> slot offset
        let entry = builder.new_label();
        let bad = builder.new_label();
        let valid = builder.new_label();
        builder.create_block(entry).ir()?;
        let owner = fat_temp(0, BankTag::Mixed);
        let missing = ptr_is_null(builder, &owner)?;
        builder.build_branch_cond(missing, bad, valid).ir()?;
        builder.create_block(bad).ir()?;
        async_fail(builder, FAIL_ASYNC_POLL)?;
        builder.create_block(valid).ir()?;
        let child = load_fat(builder, &owner, 2)?;
        let state = load_cell(builder, &child, RESUME, IrType::I16)?;
        let complete = builder
            .build_binary(
                IrBinaryOp::Eq,
                state,
                Value::Constant(ASYNC_COMPLETE),
                IrType::I1,
            )
            .ir()?;
        let start = builder.new_label();
        builder
            .build_branch_cond(Value::Temp(complete), bad, start)
            .ir()?;
        builder.create_block(start).ir()?;
        let id = load_cell(builder, &child, FUNC_ID, IrType::I16)?;
        let ready = builder.new_label();
        for body in built
            .procs
            .iter()
            .filter(|p| p.async_result.is_some() && p.ret == output)
        {
            let yes = builder.new_label();
            let no = builder.new_label();
            let eq = builder
                .build_binary(
                    IrBinaryOp::Eq,
                    id.clone(),
                    Value::Constant(body.func_id as i64),
                    IrType::I1,
                )
                .ir()?;
            builder.build_branch_cond(Value::Temp(eq), yes, no).ir()?;
            builder.create_block(yes).ir()?;
            if let Some(slot) = body.async_context {
                let ptr = gep(builder, &child, body.slots[slot].offset)?;
                builder
                    .build_store(fat_temp(1, BankTag::Mixed), ptr, ptr_ty())
                    .ir()?;
            }
            builder.build_branch(ready).ir()?;
            builder.create_block(no).ir()?;
        }
        async_fail(builder, FAIL_ASYNC_POLL)?;
        builder.create_block(ready).ir()?;
        let caller = gep(builder, &child, CALLER)?;
        builder
            .build_store(fat_temp(2, BankTag::Mixed), caller, ptr_ty())
            .ir()?;
        store_cell(builder, &child, DEST, Value::Temp(3))?;
        builder.build_return(Some(child)).ir()?;
        module.add_function(builder.finish_function().unwrap());
    }
    Ok(())
}

fn async_destination(
    builder: &mut IrBuilder,
    frame: &Value,
) -> Result<(Value, Value), CompileError> {
    let caller = load_fat(builder, frame, CALLER)?;
    let offset = load_cell(builder, frame, DEST, IrType::I16)?;
    let out = builder
        .build_pointer_offset(caller.clone(), offset, ptr_ty())
        .ir()?;
    Ok((caller, out))
}

pub(super) fn emit_async_pending(
    builder: &mut IrBuilder,
    frame: &Value,
) -> Result<(), CompileError> {
    let (caller, out) = async_destination(builder, frame)?;
    for i in 0..3 {
        store_cell(builder, &out, i, Value::Constant(0))?;
    }
    // No synchronous caller frame may remain referenced across suspension.
    let link = gep(builder, frame, CALLER)?;
    builder.build_store(null_ptr(), link, ptr_ty()).ir()?;
    builder.build_return(Some(caller)).ir()
}

pub(super) fn emit_async_ready(
    builder: &mut IrBuilder,
    proc: &Proc,
    frame: &Value,
) -> Result<(), CompileError> {
    let (_, out) = async_destination(builder, frame)?;
    let object = if let Some(result) = proc.result_slot {
        let object = allocate(builder, OBJ_FIELDS + proc.ret.cells())?;
        store_cell(builder, &object, OBJ_REFS, Value::Constant(1))?;
        store_cell(builder, &object, OBJ_TAG, Value::Constant(1))?;
        copy_to_offset(builder, proc, frame, result, &object, OBJ_FIELDS)?;
        object
    } else {
        null_ptr()
    };
    store_cell(builder, &out, 0, Value::Constant(1))?;
    let payload = gep(builder, &out, 1)?;
    builder.build_store(object, payload, ptr_ty()).ir()?;
    release_managed_slots(builder, proc, frame)?;
    let caller = load_fat(builder, frame, CALLER)?;
    let link = gep(builder, frame, CALLER)?;
    builder.build_store(null_ptr(), link, ptr_ty()).ir()?;
    store_cell(builder, frame, RESUME, Value::Constant(ASYNC_COMPLETE))?;
    builder.build_return(Some(caller)).ir()
}
