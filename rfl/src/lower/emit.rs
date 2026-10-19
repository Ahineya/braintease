//! Emit support for RFL lowering.

use super::*;

/// Dedicated entries are reached only from the function's resume dispatcher.
/// Ordinary internal branches continue to target the original segment blocks.
pub(super) type ResumeEntries = std::collections::BTreeMap<usize, u32>;

pub(super) fn emit_proc(
    builder: &mut IrBuilder,
    proc: &Proc,
    built: &Built,
) -> Result<ResumeEntries, CompileError> {
    builder.create_function(
        if proc.direct {
            direct::symbol(proc)
        } else {
            fn_symbol(&proc.name)
        },
        ptr_ty(),
    );
    builder.add_parameter(0, ptr_ty());
    if proc.direct {
        builder.add_parameter(1, ptr_ty());
    }
    let frame = fat_temp(0, BankTag::Mixed);
    // The backend treats block 0 as the function entry and emits no label for
    // it. The entry has to be the first block created in the module so that
    // id is 0, and every later block keeps an id the backend will label.
    let entry = builder.new_label();
    builder.create_block(entry).ir()?;
    let scratch = direct::scratch(builder, proc, built)?;
    if proc.segs.len() == 1 {
        emit_seg(builder, proc, built, &frame, &[entry], 0, scratch.as_ref())?;
        return Ok(ResumeEntries::from([(0, entry)]));
    }
    let labels: Vec<u32> = (0..proc.segs.len()).map(|_| builder.new_label()).collect();
    let order = segment_order(proc);
    let mut resumes = std::collections::BTreeSet::from([0]);
    for &index in &order {
        if let Some(
            Term::Call { next, .. }
            | Term::Apply { next, .. }
            | Term::Suspend { next, .. }
            | Term::AsyncPoll { next, .. },
        ) = proc.segs[index].term
        {
            if direct::target(proc, built, index).is_none() {
                resumes.insert(next);
            }
        }
    }
    let entries: ResumeEntries = resumes
        .into_iter()
        .map(|index| (index, builder.new_label()))
        .collect();
    emit_switch(builder, &frame, &entries)?;
    for (&index, &label) in &entries {
        builder.create_block(label).ir()?;
        builder.build_branch(labels[index]).ir()?;
    }
    for index in order {
        builder.create_block(labels[index]).ir()?;
        emit_seg(
            builder,
            proc,
            built,
            &frame,
            &labels,
            index,
            scratch.as_ref(),
        )?;
    }
    Ok(entries)
}

fn emit_switch(
    builder: &mut IrBuilder,
    frame: &Value,
    resumes: &ResumeEntries,
) -> Result<(), CompileError> {
    if resumes.len() == 1 {
        return builder.build_branch(resumes[&0]).ir();
    }
    let entries: Vec<_> = resumes
        .iter()
        .map(|(&index, &label)| (index as i64, label))
        .collect();
    let resume = load_cell(builder, frame, RESUME, IrType::I16)?;
    let bad = builder.new_label();
    index_switch(builder, resume, &entries, bad)?;
    builder.create_block(bad).ir()?;
    fail(builder, FAIL_UNREACHABLE)?;
    builder.build_return(Some(null_ptr())).ir()
}

pub(super) fn emit_seg(
    builder: &mut IrBuilder,
    proc: &Proc,
    built: &Built,
    frame: &Value,
    labels: &[u32],
    index: usize,
    scratch: Option<&direct::Scratch>,
) -> Result<(), CompileError> {
    if index == 0 {
        zero_managed_slots(builder, proc, frame)?;
        if !proc.captures.is_empty() {
            emit_capture_loads(builder, proc, frame)?;
        }
    }
    let consumed = consumed_temporaries(proc);
    for insn in &proc.segs[index].insns {
        emit_insn(builder, proc, built, frame, insn)?;
        for slot in operands(insn) {
            if consumed.contains(&slot) {
                release_managed(builder, proc, frame, slot)?;
            }
        }
    }
    match proc.segs[index].term.as_ref().unwrap() {
        Term::Suspend { next, .. } => {
            // Safe borrows end before await. These views (including temporary
            // poll-context copies) must not retain addresses into a prior poll.
            for slot in &proc.slots {
                if slot.ty.is_ref() {
                    for i in 0..2 {
                        store_cell(builder, frame, slot.offset + i, Value::Constant(0))?;
                    }
                }
            }
            store_cell(builder, frame, RESUME, Value::Constant(*next as i64))?;
            emit_async_pending(builder, frame)
        }
        Term::AsyncPoll {
            dest,
            future,
            context,
            next,
        } => {
            let child = emit_async_enter(builder, proc, frame, *dest, *future, *context)?;
            store_cell(builder, frame, RESUME, Value::Constant(*next as i64))?;
            builder.build_return(Some(child)).ir()
        }
        Term::Return => emit_return(builder, proc, frame, built.need_alloc),
        Term::Jump(target) => builder.build_branch(labels[*target]).ir(),
        Term::Tag {
            slot,
            tag,
            then_seg,
            else_seg,
        } => {
            let addr = load_cell(builder, frame, proc.slots[*slot].offset, IrType::I16)?;
            let same = builder
                .build_binary(
                    IrBinaryOp::Eq,
                    addr,
                    Value::Constant(*tag as i64),
                    IrType::I1,
                )
                .ir()?;
            builder
                .build_branch_cond(Value::Temp(same), labels[*then_seg], labels[*else_seg])
                .ir()
        }
        Term::If {
            cond,
            then_seg,
            else_seg,
        } => {
            let cond = load_slot(builder, proc, frame, *cond)?;
            builder
                .build_branch_cond(cond, labels[*then_seg], labels[*else_seg])
                .ir()
        }
        Term::Call {
            callee,
            args,
            dest,
            next,
        } => {
            if let Some(target) = direct::target(proc, built, index) {
                direct::call(
                    builder,
                    proc,
                    target,
                    frame,
                    args,
                    None,
                    *dest,
                    scratch.unwrap(),
                )?;
                builder.build_branch(labels[*next]).ir()
            } else {
                emit_call(builder, proc, built, frame, callee, args, *dest, *next)
            }
        }
        Term::Apply {
            slot,
            args,
            arg_tys,
            dest,
            next,
        } => {
            if let Some(target) = direct::target(proc, built, index) {
                direct::call(
                    builder,
                    proc,
                    target,
                    frame,
                    args,
                    Some(*slot),
                    *dest,
                    scratch.unwrap(),
                )?;
                return builder.build_branch(labels[*next]).ir();
            }
            let known = built.known_calls[proc.func_id]
                .get(&index)
                .and_then(|name| built.procs.iter().find(|p| p.name == *name));
            emit_apply(
                builder, proc, frame, *slot, args, arg_tys, *dest, *next, known,
            )
        }
        Term::Fail => {
            fail(builder, FAIL_MATCH)?;
            builder.build_return(Some(null_ptr())).ir()
        }
        Term::Lit {
            slot,
            value,
            then_seg,
            else_seg,
        } => {
            let ty = proc.slots[*slot].ty;
            let got = load_slot(builder, proc, frame, *slot)?;
            let same = builder
                .build_binary(
                    IrBinaryOp::Eq,
                    got,
                    Value::Constant(ty.bits(*value)),
                    IrType::I1,
                )
                .ir()?;
            builder
                .build_branch_cond(Value::Temp(same), labels[*then_seg], labels[*else_seg])
                .ir()
        }
        Term::Tail { callee, args } => {
            if proc.direct {
                let target = direct::target(proc, built, index)
                    .ok_or_else(|| CompileError::plain("unproved tail call in direct body"))?;
                direct::tail(builder, proc, target, frame, args, scratch.unwrap())
            } else {
                emit_tail_call(builder, proc, built, frame, callee, args)
            }
        }
    }
}

pub(super) fn emit_insn(
    builder: &mut IrBuilder,
    proc: &Proc,
    built: &Built,
    frame: &Value,
    insn: &Insn,
) -> Result<(), CompileError> {
    match insn {
        Insn::Expect {
            dest,
            src,
            message,
            ok_tag,
        } => emit_expect(builder, proc, frame, *dest, *src, message, *ok_tag),
        Insn::AsyncNew { .. } => emit_async_new(builder, proc, built, frame, insn),
        Insn::AsyncCancel { future } => emit_async_cancel(builder, proc, frame, *future),
        Insn::Replace { dest, place, value } => {
            emit_replace(builder, proc, frame, *dest, *place, *value)
        }
        Insn::EraseOwner { dest, src, owner } => {
            emit_erase_owner(builder, proc, frame, *dest, *src, *owner)
        }
        Insn::IntoInner { dest, src } => emit_into_inner(builder, proc, frame, *dest, *src),
        Insn::Move { dest, src } => {
            if dest == src {
                return Ok(());
            }
            release_managed(builder, proc, frame, *dest)?;
            transfer_slot(builder, proc, frame, *src, proc, frame, *dest)
        }
        Insn::Drop { slot } => release_managed(builder, proc, frame, *slot),
        Insn::Owned {
            dest,
            value,
            cleanup,
            inner,
        } => emit_owned(builder, proc, frame, *dest, *value, *cleanup, *inner),
        Insn::Const { slot, value } => {
            store_slot(builder, proc, frame, *slot, Value::Constant(*value))
        }
        Insn::CStr { dest, bytes } => emit_cstr(builder, proc, built, frame, *dest, bytes),
        Insn::Str { dest, bytes } => emit_str(builder, proc, built, frame, *dest, bytes),
        Insn::StrConcat { dest, lhs, rhs } => {
            emit_str_concat(builder, proc, frame, *dest, *lhs, *rhs)
        }
        Insn::StrSlice {
            dest,
            base,
            start,
            end,
            inclusive,
        } => emit_str_slice(builder, proc, frame, *dest, *base, *start, *end, *inclusive),
        Insn::StrFromChar { dest, src } => emit_str_from_char(builder, proc, frame, *dest, *src),
        Insn::StrFromByte { dest, src } => emit_str_from_byte(builder, proc, frame, *dest, *src),
        Insn::Extern {
            dest,
            name,
            args,
            ret,
            fixed,
        } => emit_extern(builder, proc, built, frame, *dest, name, args, *ret, *fixed),
        Insn::Copy { dest, src } => {
            if *dest != *src && keeps(proc, proc.slots[*dest].ty) {
                release_managed(builder, proc, frame, *dest)?;
            }
            if *dest != *src {
                copy_slot(builder, proc, frame, *src, proc, frame, *dest)
            } else {
                Ok(())
            }
        }
        Insn::Bin { dest, op, lhs, rhs } => emit_bin(builder, proc, frame, *dest, *op, *lhs, *rhs),
        Insn::Neg { dest, src } => {
            let slot_ty = proc.slots[*src].ty;
            if slot_ty.is_float() {
                let value = load_slot(builder, proc, frame, *src)?;
                let ir = ir_ty(slot_ty);
                let sign = if slot_ty == Ty::F32 {
                    i32_const(builder, 0x8000_0000)?
                } else {
                    i64_const(builder, i64::MIN)?
                };
                let negated = builder
                    .build_binary(IrBinaryOp::Xor, value, sign, ir)
                    .ir()?;
                return store_slot(builder, proc, frame, *dest, Value::Temp(negated));
            }
            let slot = &proc.slots[*src];
            let value = load_slot(builder, proc, frame, *src)?;
            let negated = if matches!(slot.ty, Ty::I8) {
                let wide = builder
                    .build_unary(IrUnaryOp::ZExt, value, IrType::I16)
                    .ir()?;
                let zero = Value::Constant(0);
                let sub = builder
                    .build_binary(IrBinaryOp::Sub, zero, Value::Temp(wide), IrType::I16)
                    .ir()?;
                Value::Temp(
                    builder
                        .build_unary(IrUnaryOp::Trunc, Value::Temp(sub), IrType::I8)
                        .ir()?,
                )
            } else {
                let ty = ir_ty(slot.ty);
                let zero = wide_zero(builder, ty.clone())?;
                Value::Temp(
                    builder
                        .build_binary(IrBinaryOp::Sub, zero, value, ty)
                        .ir()?,
                )
            };
            store_slot(builder, proc, frame, *dest, negated)
        }
        Insn::Not { dest, src } => {
            let value = load_slot(builder, proc, frame, *src)?;
            let flipped = builder
                .build_binary(IrBinaryOp::Eq, value, Value::Constant(0), IrType::I1)
                .ir()?;
            store_slot(builder, proc, frame, *dest, Value::Temp(flipped))
        }
        Insn::BitNot { dest, src } => {
            let ty = proc.slots[*src].ty;
            let value = load_slot(builder, proc, frame, *src)?;
            let value = widen_for_op(builder, value, ty)?;
            let flipped = builder
                .build_unary(IrUnaryOp::Not, value, arith_ty(ty))
                .ir()?;
            let flipped = if matches!(ty, Ty::I8 | Ty::U8) {
                Value::Temp(
                    builder
                        .build_unary(IrUnaryOp::Trunc, Value::Temp(flipped), IrType::I8)
                        .ir()?,
                )
            } else {
                Value::Temp(flipped)
            };
            store_slot(builder, proc, frame, *dest, flipped)
        }
        Insn::BoxNew { slot } => emit_box_new(builder, proc, frame, *slot),
        Insn::CFn { dest, symbol } => {
            let ptr = gep(builder, frame, proc.slots[*dest].offset)?;
            builder
                .build_store(Value::Function(symbol.clone()), ptr, ptr_ty())
                .ir()
        }
        Insn::CApply {
            dest,
            callee,
            args,
            ret,
        } => emit_c_apply(
            builder,
            proc,
            frame,
            *dest,
            *callee,
            args,
            *ret,
            &built.repr_c,
        ),
        Insn::Make { dest, tag, fields } => emit_make(builder, proc, frame, *dest, *tag, fields),
        Insn::Read {
            dest,
            src,
            offset,
            cells,
        } => emit_read(builder, proc, frame, *dest, *src, *offset, *cells),
        Insn::Print { slot, ty, newline } => emit_print(builder, proc, frame, *slot, *ty, *newline),
        Insn::PrintLit { chars, newline } => {
            emit_chars(builder, chars)?;
            if *newline {
                putchar(builder, Value::Constant(10))?;
            }
            Ok(())
        }
        Insn::Closure { dest, func, .. } => {
            let (func_id, frame_cells, captures) = {
                let callee = built.procs.iter().find(|item| item.name == *func).unwrap();
                (callee.func_id, callee.frame_cells, callee.captures.clone())
            };
            let (addr, bank) = if captures.is_empty() {
                (Value::Constant(0), Value::Constant(0))
            } else {
                emit_env(builder, proc, frame, &captures)?
            };
            release_managed(builder, proc, frame, *dest)?;
            let base = proc.slots[*dest].offset;
            store_cell(builder, frame, base, Value::Constant(func_id as i64))?;
            store_cell(builder, frame, base + 1, Value::Constant(frame_cells))?;
            store_cell(builder, frame, base + 2, addr)?;
            store_cell(builder, frame, base + 3, bank)
        }
        Insn::VecLit { dest, elems, elem } => {
            emit_vec_lit(builder, proc, frame, *dest, elems, *elem)
        }
        Insn::VecFill { dest, len, value } => {
            emit_vec_fill(builder, proc, frame, *dest, *len, *value)
        }
        Insn::ArrayLit { dest, elems, elem } => {
            emit_array_lit(builder, proc, frame, *dest, elems, *elem)
        }
        Insn::SliceNew {
            dest,
            base,
            start,
            end,
            inclusive,
            elem,
        } => emit_slice_new(
            builder, proc, frame, *dest, *base, *start, *end, *inclusive, *elem,
        ),
        Insn::ToVec { dest, src, elem } => emit_to_vec(builder, proc, frame, *dest, *src, *elem),
        Insn::Index {
            dest,
            vec,
            index,
            elem,
        } => emit_vec_index(builder, proc, frame, *dest, *vec, *index, *elem),
        Insn::Len { dest, vec } => emit_vec_len(builder, proc, frame, *dest, *vec),
        Insn::Empty { dest, vec } => emit_vec_empty(builder, proc, frame, *dest, *vec),
        Insn::Clear { vec, offset, elem } => {
            emit_vec_clear(builder, proc, frame, *vec, *offset, *elem)
        }
        Insn::Pop {
            vec,
            offset,
            value,
            dest,
            elem,
            some_tag,
            none_tag,
        } => emit_vec_pop(
            builder, proc, frame, *vec, *offset, *value, *dest, *elem, *some_tag, *none_tag,
        ),
        Insn::Get {
            vec,
            index,
            value,
            dest,
            elem,
            some_tag,
            none_tag,
        } => emit_vec_get(
            builder, proc, frame, *vec, *index, *value, *dest, *elem, *some_tag, *none_tag,
        ),
        Insn::Borrow {
            dest,
            place,
            mutable,
        } => emit_borrow(builder, proc, frame, *dest, place, *mutable),
        Insn::PtrLoad {
            dest,
            ptr,
            index,
            stride,
            cells,
        } => emit_ptr_load(builder, proc, frame, *dest, *ptr, *index, *stride, *cells),
        Insn::PtrStore {
            ptr,
            index,
            src,
            stride,
            cells,
        } => emit_ptr_store(builder, proc, frame, *ptr, *index, *src, *stride, *cells),
        Insn::PtrOff {
            dest,
            ptr,
            index,
            stride,
        } => emit_ptr_off(builder, proc, frame, *dest, *ptr, *index, *stride),
        Insn::PtrWord { dest, ptr, cell } => {
            let word = load_cell(builder, frame, proc.slots[*ptr].offset + cell, IrType::I16)?;
            store_slot(builder, proc, frame, *dest, word)
        }
        Insn::PtrNull { dest, ptr } => {
            let addr = load_cell(builder, frame, proc.slots[*ptr].offset, IrType::I16)?;
            let is_null = builder
                .build_binary(IrBinaryOp::Eq, addr, Value::Constant(0), IrType::I1)
                .ir()?;
            store_slot(builder, proc, frame, *dest, Value::Temp(is_null))
        }
        Insn::Try {
            src,
            dest,
            result,
            ok_tag,
            fail_tag,
            payload,
            fail_slot,
            fail,
            cleanup,
        } => emit_try(
            builder,
            proc,
            frame,
            *src,
            *dest,
            *result,
            *ok_tag,
            *fail_tag,
            *payload,
            *fail_slot,
            *fail,
            built.need_alloc,
            cleanup,
        ),
        Insn::Push {
            vec,
            offset,
            value,
            elem,
        } => emit_vec_push(builder, proc, frame, *vec, *offset, *value, *elem),
        Insn::IndexStore {
            vec,
            index,
            value,
            elem,
            offset,
            container,
        } => emit_vec_index_store(
            builder, proc, frame, *vec, *index, *value, *elem, *offset, *container,
        ),
        Insn::VecStore {
            dest,
            src,
            transfer,
        } => emit_vec_store(builder, proc, frame, *dest, *src, *transfer),
        Insn::Putc { slot } => {
            let ty = proc.slots[*slot].ty;
            let value = load_slot(builder, proc, frame, *slot)?;
            let cell = if width(ty) > 16 {
                Value::Temp(
                    builder
                        .build_unary(IrUnaryOp::Trunc, value, IrType::I16)
                        .ir()?,
                )
            } else if matches!(ty, Ty::I8 | Ty::U8) {
                Value::Temp(
                    builder
                        .build_unary(IrUnaryOp::ZExt, value, IrType::I16)
                        .ir()?,
                )
            } else {
                value
            };
            putchar(builder, cell)
        }
        Insn::Record { dest, fields } => emit_record(builder, proc, frame, *dest, fields),
        Insn::Take { dest, src, offset } => emit_take(builder, proc, frame, *dest, *src, *offset),
        Insn::FieldStore { dest, offset, src } => {
            emit_field_store(builder, proc, frame, *dest, *offset, *src)
        }
        Insn::Cast {
            dest,
            src,
            from,
            to,
        } => {
            let value = load_slot(builder, proc, frame, *src)?;
            let cast = cast_value(builder, value, *from, *to)?;
            store_slot(builder, proc, frame, *dest, cast)
        }
    }
}

pub(super) fn emit_bin(
    builder: &mut IrBuilder,
    proc: &Proc,
    frame: &Value,
    dest: usize,
    op: BinOp,
    lhs: usize,
    rhs: usize,
) -> Result<(), CompileError> {
    let ty = proc.slots[lhs].ty;
    if ty == Ty::String {
        return emit_string_cmp(builder, proc, frame, dest, op, lhs, rhs);
    }
    if ty.is_float() {
        return emit_float_bin(builder, proc, frame, dest, op, lhs, rhs, ty);
    }
    let left = load_slot(builder, proc, frame, lhs)?;
    let right = load_slot(builder, proc, frame, rhs)?;
    let left = widen_for_op(builder, left, ty)?;
    let right = widen_for_op(builder, right, ty)?;
    let ir = ir_op(op, ty);
    let result_ty = if op.is_compare() {
        IrType::I1
    } else {
        arith_ty(ty)
    };
    if matches!(op, BinOp::Div | BinOp::Rem) {
        let ok = builder.new_label();
        let bad = builder.new_label();
        let zero = wide_zero(builder, arith_ty(ty))?;
        let is_zero = builder
            .build_binary(IrBinaryOp::Eq, right.clone(), zero, IrType::I1)
            .ir()?;
        builder
            .build_branch_cond(Value::Temp(is_zero), bad, ok)
            .ir()?;
        builder.create_block(bad).ir()?;
        fail(builder, FAIL_DIV0)?;
        builder.build_return(Some(null_ptr())).ir()?;
        builder.create_block(ok).ir()?;
    }
    let value = builder.build_binary(ir, left, right, result_ty).ir()?;
    let value = if matches!(ty, Ty::I8 | Ty::U8) && (op.is_arith() || op.is_bitwise()) {
        Value::Temp(
            builder
                .build_unary(IrUnaryOp::Trunc, Value::Temp(value), IrType::I8)
                .ir()?,
        )
    } else {
        Value::Temp(value)
    };
    store_slot(builder, proc, frame, dest, value)
}

pub(super) fn emit_float_bin(
    builder: &mut IrBuilder,
    proc: &Proc,
    frame: &Value,
    dest: usize,
    op: BinOp,
    lhs: usize,
    rhs: usize,
    ty: Ty,
) -> Result<(), CompileError> {
    let left = load_slot(builder, proc, frame, lhs)?;
    let right = load_slot(builder, proc, frame, rhs)?;
    let width = if ty == Ty::F64 { "f64" } else { "f32" };
    let name = match op {
        BinOp::Add => "add",
        BinOp::Sub => "sub",
        BinOp::Mul => "mul",
        BinOp::Div => "div",
        BinOp::Eq => "eq",
        BinOp::Ne => "ne",
        BinOp::Lt => "lt",
        BinOp::Le => "le",
        BinOp::Gt => "gt",
        BinOp::Ge => "ge",
        BinOp::Rem
        | BinOp::And
        | BinOp::Or
        | BinOp::BitAnd
        | BinOp::BitOr
        | BinOp::BitXor
        | BinOp::Shl
        | BinOp::Shr => {
            return Err(CompileError::plain(format!(
                "`{op:?}` is not a float operation"
            )));
        }
    };
    let helper = format!("__rcc_{width}_{name}");
    if op.is_compare() {
        let raw = call_helper(builder, &helper, vec![left, right], IrType::I16)?;
        let flag = builder
            .build_binary(IrBinaryOp::Ne, raw, Value::Constant(0), IrType::I1)
            .ir()?;
        store_slot(builder, proc, frame, dest, Value::Temp(flag))
    } else {
        let ret = if ty == Ty::F64 {
            IrType::I64
        } else {
            IrType::I32
        };
        let raw = call_helper(builder, &helper, vec![left, right], ret)?;
        store_slot(builder, proc, frame, dest, raw)
    }
}

pub(super) fn call_helper(
    builder: &mut IrBuilder,
    name: &str,
    args: Vec<Value>,
    ret: IrType,
) -> Result<Value, CompileError> {
    let temp = if rcc_ir::float::FloatIntrinsic::from_helper(name).is_some() {
        rcc_ir::float::build_from_helper(builder, name, args, ret)
    } else {
        builder.build_call(Value::Global(name.to_string()), args, ret)
    }
    .ir()?
    .unwrap();
    Ok(Value::Temp(temp))
}
