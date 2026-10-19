//! Bounded synchronous calls. The first-class ABI remains resumable; a small
//! stack entry is used only when the complete guest call chain is known.
use super::*;
use std::collections::{HashMap, HashSet};

const MAX_FRAME: i64 = 256;
const MAX_DEPTH: usize = 4;
const MAX_CHAIN: i64 = 1024;

pub(super) fn analyze(procs: &mut [Proc], adts: &[AdtDef]) {
    let names: HashMap<_, _> = procs
        .iter()
        .enumerate()
        .map(|(i, p)| (p.name.as_str(), i))
        .collect();
    let safe: Vec<_> = procs
        .iter()
        .map(|p| {
            p.name != "main"
                && !p.c_export
                && p.async_result.is_none()
                && p.frame_cells <= MAX_FRAME
                && p.slots
                    .iter()
                    .all(|s| quiet_drop(p, s.ty, adts, &mut HashSet::new()))
                && p.segs.iter().all(|s| {
                    !matches!(
                        s.term,
                        Some(Term::Apply { .. } | Term::Suspend { .. } | Term::AsyncPoll { .. })
                    ) && s.insns.iter().all(|i| {
                        !matches!(
                            i,
                            Insn::Extern { .. }
                                | Insn::CApply { .. }
                                | Insn::AsyncNew { .. }
                                | Insn::AsyncCancel { .. }
                        )
                    })
                })
        })
        .collect();
    let mut proofs = vec![None; procs.len()];
    loop {
        let mut changed = false;
        for (i, p) in procs.iter().enumerate() {
            if !safe[i] || proofs[i].is_some() {
                continue;
            }
            let mut depth = 1;
            let mut cost = p.frame_cells + p.ret.cells();
            let mut child_cost = 0;
            let mut ready = true;
            for s in &p.segs {
                if let Some(Term::Call { callee, .. } | Term::Tail { callee, .. }) = &s.term {
                    let Some(&(d, c)) =
                        names.get(callee.as_str()).and_then(|i| proofs[*i].as_ref())
                    else {
                        ready = false;
                        break;
                    };
                    depth = depth.max(d + 1);
                    child_cost = child_cost.max(c);
                }
            }
            cost += child_cost;
            if ready && depth <= MAX_DEPTH && cost <= MAX_CHAIN {
                proofs[i] = Some((depth, cost));
                changed = true;
            }
        }
        if !changed {
            break;
        }
    }
    for (p, proof) in procs.iter_mut().zip(proofs) {
        p.direct = proof.is_some();
    }
}

fn quiet_drop(p: &Proc, ty: Ty, adts: &[AdtDef], seen: &mut HashSet<Ty>) -> bool {
    if !seen.insert(ty) {
        return true;
    }
    match ty {
        Ty::Func(_) | Ty::Owned(_) | Ty::ErasedOwner | Ty::SharedOwner => false,
        Ty::Struct { id, .. } => p.structs[id as usize]
            .iter()
            .all(|t| quiet_drop(p, *t, adts, seen)),
        Ty::Tuple { id, .. } => p.tuples[id as usize]
            .iter()
            .all(|t| quiet_drop(p, *t, adts, seen)),
        Ty::Array { id, .. } => quiet_drop(p, p.collections.arrays[id as usize], adts, seen),
        Ty::Vec(id) => quiet_drop(p, p.collections.vectors[id as usize], adts, seen),
        Ty::Slice(id) => quiet_drop(p, p.collections.slices[id as usize], adts, seen),
        Ty::Adt(id) => adts[id as usize]
            .ctors
            .iter()
            .flat_map(|c| &c.fields)
            .all(|t| quiet_drop(p, *t, adts, seen)),
        _ => true,
    }
}

pub(super) fn symbol(p: &Proc) -> String {
    format!("rfl_direct_{}", p.name)
}

pub(super) fn target<'a>(proc: &Proc, built: &'a Built, index: usize) -> Option<&'a Proc> {
    let name = match proc.segs[index].term.as_ref()? {
        Term::Call { callee, .. } | Term::Tail { callee, .. } => callee,
        Term::Apply { .. } => built.known_calls[proc.func_id].get(&index)?,
        _ => return None,
    };
    built.procs.iter().find(|p| p.name == *name && p.direct)
}

pub(super) struct Scratch {
    frame: Value,
    out: Value,
}

pub(super) fn scratch(
    builder: &mut IrBuilder,
    proc: &Proc,
    built: &Built,
) -> Result<Option<Scratch>, CompileError> {
    let targets: Vec<_> = (0..proc.segs.len())
        .filter(|i| proc.direct || !matches!(proc.segs[*i].term, Some(Term::Tail { .. })))
        .filter_map(|i| target(proc, built, i))
        .collect();
    let Some(size) = targets.iter().map(|p| p.frame_cells).max() else {
        return Ok(None);
    };
    let result = targets
        .iter()
        .map(|p| p.ret.cells())
        .max()
        .unwrap_or(0)
        .max(1);
    let frame = builder
        .build_alloca(IrType::I16, Some(Value::Constant(size)))
        .ir()?;
    let out = builder
        .build_alloca(IrType::I16, Some(Value::Constant(result)))
        .ir()?;
    Ok(Some(Scratch { frame, out }))
}

fn prepare(
    builder: &mut IrBuilder,
    proc: &Proc,
    callee: &Proc,
    frame: &Value,
    args: &[usize],
    closure: Option<usize>,
    scratch: &Scratch,
) -> Result<(), CompileError> {
    for (&arg, &param) in args.iter().zip(&callee.param_slots) {
        copy_slot(builder, proc, frame, arg, callee, &scratch.frame, param)?;
    }
    if !callee.captures.is_empty() {
        let slot = closure
            .ok_or_else(|| CompileError::plain("capturing direct call has no environment"))?;
        let env = load_fat(builder, frame, proc.slots[slot].offset + 2)?;
        let at = gep(builder, &scratch.frame, callee.env_at.unwrap())?;
        builder.build_store(env, at, ptr_ty()).ir()?;
    }
    Ok(())
}

pub(super) fn call(
    builder: &mut IrBuilder,
    proc: &Proc,
    callee: &Proc,
    frame: &Value,
    args: &[usize],
    closure: Option<usize>,
    dest: usize,
    scratch: &Scratch,
) -> Result<(), CompileError> {
    prepare(builder, proc, callee, frame, args, closure, scratch)?;
    builder
        .build_call(
            Value::Global(symbol(callee)),
            vec![scratch.frame.clone(), scratch.out.clone()],
            ptr_ty(),
        )
        .ir()?;
    if proc.slots[dest].ty == Ty::Unit {
        return Ok(());
    }
    if cell_wise(proc.slots[dest].ty) {
        release_managed(builder, proc, frame, dest)?;
        copy_frame_cells(
            builder,
            &scratch.out,
            0,
            frame,
            proc.slots[dest].offset,
            proc.slots[dest].ty.cells(),
        )?;
    } else {
        let value = load_cell(builder, &scratch.out, 0, ir_ty(proc.slots[dest].ty))?;
        store_slot(builder, proc, frame, dest, value)?;
    }
    Ok(())
}

pub(super) fn tail(
    builder: &mut IrBuilder,
    proc: &Proc,
    callee: &Proc,
    frame: &Value,
    args: &[usize],
    scratch: &Scratch,
) -> Result<(), CompileError> {
    prepare(builder, proc, callee, frame, args, None, scratch)?;
    release_managed_slots(builder, proc, frame)?;
    builder
        .build_call(
            Value::Global(symbol(callee)),
            vec![scratch.frame.clone(), fat_temp(1, BankTag::Mixed)],
            ptr_ty(),
        )
        .ir()?;
    builder.build_return(Some(null_ptr())).ir()
}

/// Transfer the result before frame cleanup. Private output scratch has no
/// previous owner, so this needs neither a retain nor a matching release.
pub(super) fn finish(
    builder: &mut IrBuilder,
    proc: &Proc,
    frame: &Value,
) -> Result<(), CompileError> {
    if let Some(result) = proc.result_slot {
        let out = fat_temp(1, BankTag::Mixed);
        moves::transfer_to_offset(builder, proc, frame, result, &out, 0)?;
    }
    release_managed_slots(builder, proc, frame)?;
    builder.build_return(Some(null_ptr())).ir()
}

/// Preserve the original closure/driver ABI for indirect invocation.
pub(super) fn wrapper(
    builder: &mut IrBuilder,
    proc: &Proc,
    release: bool,
) -> Result<(), CompileError> {
    builder.create_function(fn_symbol(&proc.name), ptr_ty());
    builder.add_parameter(0, ptr_ty());
    let entry = builder.new_label();
    builder.create_block(entry).ir()?;
    let frame = fat_temp(0, BankTag::Mixed);
    let out = builder
        .build_alloca(IrType::I16, Some(Value::Constant(proc.ret.cells().max(1))))
        .ir()?;
    builder
        .build_call(
            Value::Global(symbol(proc)),
            vec![frame.clone(), out.clone()],
            ptr_ty(),
        )
        .ir()?;
    let caller = load_fat(builder, &frame, CALLER)?;
    if proc.ret != Ty::Unit {
        let offset = load_cell(builder, &frame, DEST, IrType::I16)?;
        let at = builder
            .build_pointer_offset(caller.clone(), offset, ptr_ty())
            .ir()?;
        release_managed_through(builder, proc, &at, proc.ret)?;
        copy_frame_cells(builder, &out, 0, &at, 0, proc.ret.cells())?;
    }
    let saved = builder.build_alloca(ptr_ty(), None).ir()?;
    builder.build_store(caller, saved.clone(), ptr_ty()).ir()?;
    if release {
        builder
            .build_call(
                Value::Global("rfl_frame_release".into()),
                vec![frame],
                IrType::Void,
            )
            .ir()?;
    }
    let caller = load_ptr_slot(builder, &saved)?;
    builder.build_return(Some(caller)).ir()
}
