//! Balanced static dispatch. Only suspension points are valid resume entries.

use super::*;

pub(super) fn index_switch(
    builder: &mut IrBuilder,
    key: Value,
    entries: &[(i64, u32)],
    bad: u32,
) -> Result<(), CompileError> {
    if entries.is_empty() {
        return builder.build_branch(bad).ir();
    }
    if let [(value, target)] = entries {
        let same = builder
            .build_binary(IrBinaryOp::Eq, key, Value::Constant(*value), IrType::I1)
            .ir()?;
        return builder
            .build_branch_cond(Value::Temp(same), *target, bad)
            .ir();
    }
    let mid = entries.len() / 2;
    let left = builder.new_label();
    let right = builder.new_label();
    let less = builder
        .build_binary(
            IrBinaryOp::Ult,
            key.clone(),
            Value::Constant(entries[mid].0),
            IrType::I1,
        )
        .ir()?;
    builder
        .build_branch_cond(Value::Temp(less), left, right)
        .ir()?;
    builder.create_block(left).ir()?;
    index_switch(builder, key.clone(), &entries[..mid], bad)?;
    builder.create_block(right).ir()?;
    index_switch(builder, key, &entries[mid..], bad)
}
