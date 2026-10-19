//! Checked C16 logical memory. Ordinary RAM is private Wasm linear memory;
//! bank zero remains devices, and code tokens are never data addresses.
use wasm_encoder::{BlockType, Function, Instruction as W, MemArg, ValType};
pub(crate) const BANK: u64 = 64000;
pub(crate) const CELLS: u64 = 256 * BANK;
pub(crate) const STACK: u32 = 0;
pub(crate) const FAULT: u32 = 1;
pub(crate) const EXIT: u32 = 2;

pub(crate) fn fault(body: &mut Function, code: i32) {
    body.instruction(&W::I32Const(code));
    body.instruction(&W::GlobalSet(FAULT));
    body.instruction(&W::Unreachable);
}
fn guard(body: &mut Function, code: i32) {
    body.instruction(&W::If(BlockType::Empty));
    fault(body, code);
    body.instruction(&W::End);
}
pub(crate) fn memarg() -> MemArg {
    MemArg {
        offset: 0,
        align: 1,
        memory_index: 0,
    }
}

pub(crate) fn address() -> Function {
    // (logical pointer, cell count) -> byte offset. Permit normalized wide
    // accesses across banks, but reject invalid encoded offsets and code tokens.
    let mut f = Function::new([(2, ValType::I64)]);
    for i in [
        W::LocalGet(0),
        W::I64Const(16),
        W::I64ShrU,
        W::LocalTee(2),
        W::I64Eqz,
        W::LocalGet(2),
        W::I64Const(256),
        W::I64GeU,
        W::I32Or,
        W::LocalGet(0),
        W::I64Const(65535),
        W::I64And,
        W::LocalTee(3),
        W::I64Const(BANK as i64),
        W::I64GeU,
        W::I32Or,
    ] {
        f.instruction(&i);
    }
    guard(&mut f, 1);
    for i in [
        W::LocalGet(2),
        W::I64Const(BANK as i64),
        W::I64Mul,
        W::LocalGet(3),
        W::I64Add,
        W::LocalSet(2),
        W::LocalGet(1),
        W::I64Const(CELLS as i64),
        W::I64GtU,
    ] {
        f.instruction(&i);
    }
    guard(&mut f, 1);
    for i in [
        W::LocalGet(2),
        W::LocalGet(1),
        W::I64Add,
        W::I64Const(CELLS as i64),
        W::I64GtU,
    ] {
        f.instruction(&i);
    }
    guard(&mut f, 1);
    for i in [W::LocalGet(2), W::I64Const(2), W::I64Mul, W::End] {
        f.instruction(&i);
    }
    f
}
pub(crate) fn gep(address: u32) -> Function {
    let mut f = Function::new([(1, ValType::I64)]);
    for i in [
        W::LocalGet(0),
        W::I64Const(0),
        W::Call(address),
        W::I64Const(2),
        W::I64DivU,
        W::LocalSet(2),
        W::LocalGet(1),
        W::I64Const(-(CELLS as i64)),
        W::I64LtS,
        W::LocalGet(1),
        W::I64Const(CELLS as i64),
        W::I64GtS,
        W::I32Or,
    ] {
        f.instruction(&i);
    }
    guard(&mut f, 2);
    for i in [
        W::LocalGet(2),
        W::LocalGet(1),
        W::I64Add,
        W::LocalTee(2),
        W::I64Const(BANK as i64),
        W::I64LtS,
        W::LocalGet(2),
        W::I64Const(CELLS as i64),
        W::I64GeS,
        W::I32Or,
    ] {
        f.instruction(&i);
    }
    guard(&mut f, 2);
    for i in [
        W::LocalGet(2),
        W::I64Const(BANK as i64),
        W::I64DivU,
        W::I64Const(16),
        W::I64Shl,
        W::LocalGet(2),
        W::I64Const(BANK as i64),
        W::I64RemU,
        W::I64Or,
        W::End,
    ] {
        f.instruction(&i);
    }
    f
}
