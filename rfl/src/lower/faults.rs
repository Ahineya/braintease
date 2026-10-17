//! Faults support for RFL lowering.

use super::*;

pub(super) const FAIL_BOUNDS: i64 = 1;
pub(super) const FAIL_RANGE: i64 = 2;
pub(super) const FAIL_OVERFLOW: i64 = 3;
pub(super) const FAIL_ALLOC: i64 = 4;
pub(super) const FAIL_DIV0: i64 = 5;
pub(super) const FAIL_CHAR: i64 = 6;
pub(super) const FAIL_MATCH: i64 = 7;
pub(super) const FAIL_UNREACHABLE: i64 = 8;
pub(super) const FAIL_NULL: i64 = 9;
pub(super) const FAIL_ASYNC_POLL: i64 = 10;

pub(super) fn fail(builder: &mut IrBuilder, code: i64) -> Result<(), CompileError> {
    builder
        .build_call(
            Value::Global("rfl_fail".to_string()),
            vec![Value::Constant(code)],
            IrType::Void,
        )
        .ir()?;
    Ok(())
}

pub(super) fn runtime_fail(builder: &mut IrBuilder, code: i64) -> Result<(), CompileError> {
    fail(builder, code)?;
    builder.build_return(Some(null_ptr())).ir()
}

pub(super) fn emit_rfl_fail(builder: &mut IrBuilder) -> Result<(), CompileError> {
    builder.create_function("rfl_fail".to_string(), IrType::Void);
    builder.add_parameter(0, IrType::I16);
    let entry = builder.new_label();
    let done = builder.new_label();
    builder.create_block(entry).ir()?;
    let code = Value::Temp(0);
    let messages = [
        (FAIL_BOUNDS, "error: index out of bounds\n"),
        (FAIL_RANGE, "error: slice range out of bounds\n"),
        (FAIL_OVERFLOW, "error: integer overflow\n"),
        (FAIL_ALLOC, "error: allocation failed\n"),
        (FAIL_DIV0, "error: division by zero\n"),
        (FAIL_CHAR, "error: invalid character\n"),
        (FAIL_MATCH, "error: non-exhaustive match\n"),
        (FAIL_UNREACHABLE, "error: unreachable state\n"),
        (FAIL_NULL, "error: null pointer\n"),
        (
            FAIL_ASYNC_POLL,
            "error: future polled after completion or cancellation\n",
        ),
    ];
    for (id, text) in messages {
        let print = builder.new_label();
        let next = builder.new_label();
        let same = builder
            .build_binary(
                IrBinaryOp::Eq,
                code.clone(),
                Value::Constant(id),
                IrType::I1,
            )
            .ir()?;
        builder
            .build_branch_cond(Value::Temp(same), print, next)
            .ir()?;
        builder.create_block(print).ir()?;
        emit_text(builder, text)?;
        builder.build_branch(done).ir()?;
        builder.create_block(next).ir()?;
    }
    emit_text(builder, "error: runtime error\n")?;
    builder.build_branch(done).ir()?;
    builder.create_block(done).ir()?;
    builder
        .build_call(
            Value::Global("rfl_halt".to_string()),
            Vec::new(),
            IrType::Void,
        )
        .ir()?;
    builder.build_return(None).ir()
}

pub(super) fn emit_text(builder: &mut IrBuilder, text: &str) -> Result<(), CompileError> {
    for byte in text.bytes() {
        putchar(builder, Value::Constant(i64::from(byte)))?;
    }
    Ok(())
}
