//! Shared string operations. Loop bodies are emitted once per program rather
//! than expanded at every source expression (which can overflow a code bank).

use super::*;

pub(super) fn emit_string_runtime(
    builder: &mut IrBuilder,
    module: &mut Module,
    built: &Built,
) -> Result<(), CompileError> {
    for name in [
        "rfl_string_concat",
        "rfl_string_slice",
        "rfl_string_char",
        "rfl_string_equal",
        "rfl_string_literal",
    ] {
        let needed = built.procs.iter().any(|proc| {
            proc.segs.iter().any(|seg| {
                seg.insns.iter().any(|insn| match (name, insn) {
                    ("rfl_string_concat", Insn::StrConcat { .. })
                    | ("rfl_string_slice", Insn::StrSlice { .. })
                    | ("rfl_string_char", Insn::StrFromChar { .. }) => true,
                    ("rfl_string_literal", Insn::Str { bytes, .. }) => bytes.len() >= 8,
                    ("rfl_string_equal", Insn::Bin { lhs, .. }) => {
                        proc.slots[*lhs].ty == Ty::String
                    }
                    _ => false,
                })
            })
        });
        if !needed {
            continue;
        }
        match name {
            "rfl_string_concat" => concat(builder)?,
            "rfl_string_slice" => slice(builder)?,
            "rfl_string_char" => {
                builder.create_function(name.to_string(), ptr_ty());
                builder.add_parameter(0, IrType::I32);
                let entry = builder.new_label();
                builder.create_block(entry).ir()?;
                let obj = build_char_string(builder, Value::Temp(0))?;
                builder.build_return(Some(obj)).ir()?;
            }
            "rfl_string_literal" => literal(builder)?,
            _ => equal(builder)?,
        }
        module.add_function(builder.finish_function().unwrap());
    }
    Ok(())
}

// Emit this shared copy in IR rather than calling libc memcpy: standalone and
// guest libc have different size_t ABIs. The RFL count is always 32 bits.
fn literal(builder: &mut IrBuilder) -> Result<(), CompileError> {
    start(builder, "rfl_string_literal", ptr_ty(), &[ptr_ty(), COUNT])?;
    let source = fat_temp(0, BankTag::Mixed);
    let count = Value::Temp(1);
    let obj = alloc_string(builder, count.clone())?;
    let index_slot = builder.build_alloca(COUNT, None).ir()?;
    builder
        .build_store(Value::Constant(0), index_slot.clone(), COUNT)
        .ir()?;
    let head = builder.new_label();
    let body = builder.new_label();
    let done = builder.new_label();
    builder.build_branch(head).ir()?;
    builder.create_block(head).ir()?;
    let index = Value::Temp(builder.build_load(index_slot.clone(), COUNT).ir()?);
    let more = builder
        .build_binary(IrBinaryOp::Ult, index.clone(), count, IrType::I1)
        .ir()?;
    builder
        .build_branch_cond(Value::Temp(more), body, done)
        .ir()?;
    builder.create_block(body).ir()?;
    let from = builder
        .build_pointer_offset(source, index.clone(), ptr_ty())
        .ir()?;
    let byte = Value::Temp(builder.build_load(from, IrType::I16).ir()?);
    let offset = builder
        .build_binary(
            IrBinaryOp::Add,
            index.clone(),
            Value::Constant(STR_DATA),
            COUNT,
        )
        .ir()?;
    let to = builder
        .build_pointer_offset(obj.clone(), Value::Temp(offset), ptr_ty())
        .ir()?;
    builder.build_store(byte, to, IrType::I16).ir()?;
    let next = builder
        .build_binary(IrBinaryOp::Add, index, Value::Constant(1), COUNT)
        .ir()?;
    builder
        .build_store(Value::Temp(next), index_slot, COUNT)
        .ir()?;
    builder.build_branch(head).ir()?;
    builder.create_block(done).ir()?;
    builder.build_return(Some(obj)).ir()
}

fn start(
    builder: &mut IrBuilder,
    name: &str,
    ret: IrType,
    params: &[IrType],
) -> Result<(), CompileError> {
    builder.create_function(name.to_string(), ret);
    for (i, ty) in params.iter().enumerate() {
        builder.add_parameter(i as u32, ty.clone());
    }
    let entry = builder.new_label();
    builder.create_block(entry).ir()?;
    Ok(())
}

fn raw_len(builder: &mut IrBuilder, string: &Value) -> Result<Value, CompileError> {
    let empty = builder.new_label();
    let full = builder.new_label();
    let done = builder.new_label();
    let out = builder.build_alloca(COUNT, None).ir()?;
    let missing = ptr_is_null(builder, string)?;
    builder.build_branch_cond(missing, empty, full).ir()?;
    builder.create_block(empty).ir()?;
    builder
        .build_store(Value::Constant(0), out.clone(), COUNT)
        .ir()?;
    builder.build_branch(done).ir()?;
    builder.create_block(full).ir()?;
    let len = load_count(builder, string, VEC_LEN)?;
    builder.build_store(len, out.clone(), COUNT).ir()?;
    builder.build_branch(done).ir()?;
    builder.create_block(done).ir()?;
    Ok(Value::Temp(builder.build_load(out, COUNT).ir()?))
}

fn raw_byte(builder: &mut IrBuilder, string: &Value, index: Value) -> Result<Value, CompileError> {
    let offset = builder
        .build_binary(IrBinaryOp::Add, index, Value::Constant(STR_DATA), COUNT)
        .ir()?;
    let ptr = builder
        .build_pointer_offset(string.clone(), Value::Temp(offset), ptr_ty())
        .ir()?;
    Ok(Value::Temp(builder.build_load(ptr, IrType::I16).ir()?))
}

fn copy_bytes(
    builder: &mut IrBuilder,
    src: &Value,
    start: Value,
    dest: &Value,
    dest_start: Value,
    count: Value,
) -> Result<(), CompileError> {
    let slot = builder.build_alloca(COUNT, None).ir()?;
    builder
        .build_store(Value::Constant(0), slot.clone(), COUNT)
        .ir()?;
    let head = builder.new_label();
    let body = builder.new_label();
    let done = builder.new_label();
    builder.build_branch(head).ir()?;
    builder.create_block(head).ir()?;
    let index = Value::Temp(builder.build_load(slot.clone(), COUNT).ir()?);
    let more = builder
        .build_binary(IrBinaryOp::Ult, index.clone(), count, IrType::I1)
        .ir()?;
    builder
        .build_branch_cond(Value::Temp(more), body, done)
        .ir()?;
    builder.create_block(body).ir()?;
    let from = builder
        .build_binary(IrBinaryOp::Add, start, index.clone(), COUNT)
        .ir()?;
    let byte = raw_byte(builder, src, Value::Temp(from))?;
    let to = builder
        .build_binary(IrBinaryOp::Add, dest_start, index.clone(), COUNT)
        .ir()?;
    let offset = builder
        .build_binary(
            IrBinaryOp::Add,
            Value::Temp(to),
            Value::Constant(STR_DATA),
            COUNT,
        )
        .ir()?;
    let ptr = builder
        .build_pointer_offset(dest.clone(), Value::Temp(offset), ptr_ty())
        .ir()?;
    builder.build_store(byte, ptr, IrType::I16).ir()?;
    let next = builder
        .build_binary(IrBinaryOp::Add, index, Value::Constant(1), COUNT)
        .ir()?;
    builder.build_store(Value::Temp(next), slot, COUNT).ir()?;
    builder.build_branch(head).ir()?;
    builder.create_block(done).ir()?;
    Ok(())
}

fn concat(builder: &mut IrBuilder) -> Result<(), CompileError> {
    start(
        builder,
        "rfl_string_concat",
        ptr_ty(),
        &[ptr_ty(), ptr_ty()],
    )?;
    let a = fat_temp(0, BankTag::Mixed);
    let b = fat_temp(1, BankTag::Mixed);
    let left = raw_len(builder, &a)?;
    let right = raw_len(builder, &b)?;
    let total = Value::Temp(
        builder
            .build_binary(IrBinaryOp::Add, left.clone(), right.clone(), COUNT)
            .ir()?,
    );
    let overflow = builder
        .build_binary(IrBinaryOp::Ult, total.clone(), left.clone(), IrType::I1)
        .ir()?;
    let bad = builder.new_label();
    let ok = builder.new_label();
    builder
        .build_branch_cond(Value::Temp(overflow), bad, ok)
        .ir()?;
    builder.create_block(bad).ir()?;
    fail(builder, FAIL_OVERFLOW)?;
    builder.build_return(Some(null_ptr())).ir()?;
    builder.create_block(ok).ir()?;
    let obj = alloc_string(builder, total)?;
    copy_bytes(
        builder,
        &a,
        Value::Constant(0),
        &obj,
        Value::Constant(0),
        left.clone(),
    )?;
    copy_bytes(builder, &b, Value::Constant(0), &obj, left, right)?;
    builder.build_return(Some(obj)).ir()
}

fn slice(builder: &mut IrBuilder) -> Result<(), CompileError> {
    start(
        builder,
        "rfl_string_slice",
        ptr_ty(),
        &[ptr_ty(), COUNT, COUNT],
    )?;
    let source = fat_temp(0, BankTag::Mixed);
    let len = raw_len(builder, &source)?;
    check_range(builder, Value::Temp(1), Value::Temp(2), len)?;
    let count = Value::Temp(
        builder
            .build_binary(IrBinaryOp::Sub, Value::Temp(2), Value::Temp(1), COUNT)
            .ir()?,
    );
    let obj = alloc_string(builder, count.clone())?;
    copy_bytes(
        builder,
        &source,
        Value::Temp(1),
        &obj,
        Value::Constant(0),
        count,
    )?;
    builder.build_return(Some(obj)).ir()
}

fn equal(builder: &mut IrBuilder) -> Result<(), CompileError> {
    start(
        builder,
        "rfl_string_equal",
        IrType::I16,
        &[ptr_ty(), ptr_ty()],
    )?;
    let a = fat_temp(0, BankTag::Mixed);
    let b = fat_temp(1, BankTag::Mixed);
    let left = raw_len(builder, &a)?;
    let right = raw_len(builder, &b)?;
    let same = builder
        .build_binary(IrBinaryOp::Eq, left.clone(), right, IrType::I1)
        .ir()?;
    let scan = builder.new_label();
    let no = builder.new_label();
    let yes = builder.new_label();
    builder
        .build_branch_cond(Value::Temp(same), scan, no)
        .ir()?;
    builder.create_block(scan).ir()?;
    let index_slot = builder.build_alloca(COUNT, None).ir()?;
    builder
        .build_store(Value::Constant(0), index_slot.clone(), COUNT)
        .ir()?;
    let head = builder.new_label();
    let body = builder.new_label();
    builder.build_branch(head).ir()?;
    builder.create_block(head).ir()?;
    let index = Value::Temp(builder.build_load(index_slot.clone(), COUNT).ir()?);
    let end = builder
        .build_binary(IrBinaryOp::Eq, index.clone(), left, IrType::I1)
        .ir()?;
    builder
        .build_branch_cond(Value::Temp(end), yes, body)
        .ir()?;
    builder.create_block(body).ir()?;
    let x = raw_byte(builder, &a, index.clone())?;
    let y = raw_byte(builder, &b, index.clone())?;
    let same = builder
        .build_binary(IrBinaryOp::Eq, x, y, IrType::I1)
        .ir()?;
    let step = builder.new_label();
    builder
        .build_branch_cond(Value::Temp(same), step, no)
        .ir()?;
    builder.create_block(step).ir()?;
    let next = builder
        .build_binary(IrBinaryOp::Add, index, Value::Constant(1), COUNT)
        .ir()?;
    builder
        .build_store(Value::Temp(next), index_slot, COUNT)
        .ir()?;
    builder.build_branch(head).ir()?;
    builder.create_block(no).ir()?;
    builder.build_return(Some(Value::Constant(0))).ir()?;
    builder.create_block(yes).ir()?;
    builder.build_return(Some(Value::Constant(1))).ir()
}
