//! Bulk initialization of a fresh vector avoids per-push capacity and ARC work.
use super::*;

pub(super) fn emit_vec_fill(
    builder: &mut IrBuilder,
    proc: &Proc,
    frame: &Value,
    dest: usize,
    len: u32,
    value: i64,
) -> Result<(), CompileError> {
    release_managed(builder, proc, frame, dest)?;
    let count = Value::Constant(i64::from(len));
    let obj = alloc_vec(builder, count.clone(), count.clone(), 1)?;
    let data = gep(builder, &obj, VEC_DATA)?;
    let size = i32_const(builder, i64::from(len))?;
    builder
        .build_call(
            Value::Global("memset".into()),
            vec![data, Value::Constant(value), size],
            ptr_ty(),
        )
        .ir()?;
    let header = gep(builder, frame, proc.slots[dest].offset)?;
    store_vec_ptr(builder, &header, &obj)
}
