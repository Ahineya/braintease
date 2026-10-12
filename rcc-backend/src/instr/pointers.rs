//! Pointer representation operations, implemented entirely with the existing ISA.
//!
//! uintptr_t stores (bank << 16) | offset. Runtime provenance tags are resolved
//! before conversion; the null tag maps to zero. Linear addresses are used only
//! for pointer differences and general arithmetic, so integer round-trips retain
//! the bank without performing division on every comparison.
use crate::naming::NameGenerator;
use crate::regmgmt::{BankInfo, BankTagValue, RegisterPressureManager};
use rcc_codegen::{AsmInst, Reg};
use rcc_common::TempId;
use rcc_frontend::{BankTag, ir::{Value, IrBinaryOp}};
use super::{helpers, wide};

fn reg(mgr: &mut RegisterPressureManager, naming: &mut NameGenerator, insts: &mut Vec<AsmInst>, tag: &str) -> Reg {
    let r = mgr.get_register(naming.temp_with_context("pointer", tag));
    insts.extend(mgr.take_instructions());
    r
}

/// Return pinned copies, never overwrite a live pointer or its bank metadata.
pub fn components(mgr: &mut RegisterPressureManager, naming: &mut NameGenerator,
    ptr: &Value, insts: &mut Vec<AsmInst>) -> (Reg, Reg) {
    let null = matches!(ptr, Value::FatPtr(fp) if fp.bank == BankTag::Null) || matches!(ptr, Value::Constant(0));
    if null {
        let lo = reg(mgr, naming, insts, "null_addr"); mgr.pin_register(lo);
        let hi = reg(mgr, naming, insts, "null_bank"); mgr.pin_register(hi);
        insts.push(AsmInst::Li(lo, 0)); insts.push(AsmInst::Li(hi, 0));
        return (lo, hi);
    }
    let id = naming.new_internal_temp();
    let (addr, name, ai) = helpers::get_pointer_address_and_name(ptr, mgr, naming, id);
    insts.extend(ai); mgr.pin_register(addr);
    let bank_info = match ptr {
        Value::FatPtr(fp) => helpers::resolve_bank_tag_to_info(&fp.bank, fp, mgr, naming),
        _ => mgr.get_pointer_bank(&name).expect("pointer must carry its bank"),
    };
    let (bank, bi) = helpers::get_bank_register_with_runtime_check_safe(&bank_info, mgr, naming, "pointer_value");
    insts.extend(bi); mgr.pin_register(bank);
    let lo = reg(mgr, naming, insts, "addr_copy"); mgr.pin_register(lo);
    let hi = reg(mgr, naming, insts, "bank_copy"); mgr.pin_register(hi);
    insts.push(AsmInst::Add(lo, addr, Reg::R0));
    insts.push(AsmInst::Add(hi, bank, Reg::R0));
    mgr.unpin_register(addr); mgr.unpin_register(bank);
    let tag = reg(mgr, naming, insts, "null_tag");
    insts.push(AsmInst::Li(tag, BankTagValue::NULL));
    let done = naming.i32_label("pointer_nonnull");
    insts.push(AsmInst::Bne(hi, tag, done.clone()));
    insts.push(AsmInst::Li(lo, 0)); insts.push(AsmInst::Li(hi, 0));
    insts.push(AsmInst::Label(done));
    (lo, hi)
}

pub fn to_integer(mgr: &mut RegisterPressureManager, naming: &mut NameGenerator,
    ptr: &Value, result: TempId) -> Vec<AsmInst> {
    let mut insts = Vec::new();
    let (lo, hi) = components(mgr, naming, ptr, &mut insts);
    wide::bind_i32(mgr, naming, &naming.temp_name(result), lo, hi);
    mgr.unpin_register(lo); mgr.unpin_register(hi);
    insts
}

pub fn from_integer(mgr: &mut RegisterPressureManager, naming: &mut NameGenerator,
    value: &Value, result: TempId) -> Vec<AsmInst> {
    let (lo, hi, mut insts) = wide::get_i32_pair(mgr, naming, value, false);
    mgr.pin_register(lo); mgr.pin_register(hi);
    let addr = reg(mgr, naming, &mut insts, "cast_addr"); mgr.pin_register(addr);
    let bank_name = naming.temp_with_context("pointer", "cast_bank");
    let bank = mgr.get_register(bank_name.clone()); insts.extend(mgr.take_instructions()); mgr.pin_register(bank);
    insts.push(AsmInst::Add(addr, lo, Reg::R0)); insts.push(AsmInst::Add(bank, hi, Reg::R0));
    let both = reg(mgr, naming, &mut insts, "cast_null");
    insts.push(AsmInst::Or(both, addr, bank));
    let done = naming.i32_label("cast_nonnull");
    insts.push(AsmInst::Bne(both, Reg::R0, done.clone()));
    insts.push(AsmInst::Li(bank, BankTagValue::NULL));
    insts.push(AsmInst::Label(done));
    let name = naming.temp_name(result);
    mgr.bind_value_to_register(name.clone(), addr);
    mgr.set_pointer_bank(name, BankInfo::Dynamic(bank_name));
    mgr.unpin_register(lo); mgr.unpin_register(hi); mgr.unpin_register(addr); mgr.unpin_register(bank);
    insts
}

fn word_value(mgr: &mut RegisterPressureManager, naming: &mut NameGenerator,
    r: Reg, insts: &mut Vec<AsmInst>) -> Value {
    let id = naming.new_internal_temp();
    let copy = mgr.get_register(naming.temp_name(id)); insts.extend(mgr.take_instructions()); mgr.pin_register(copy);
    insts.push(AsmInst::Add(copy, r, Reg::R0));
    let high = reg(mgr, naming, insts, "word_high");
    insts.push(AsmInst::Li(high, 0));
    wide::bind_i32(mgr, naming, &naming.temp_name(id), copy, high);
    mgr.unpin_register(copy);
    Value::Temp(id)
}

pub fn to_linear(mgr: &mut RegisterPressureManager, naming: &mut NameGenerator,
    ptr: &Value, result: TempId, bank_size: u16) -> Vec<AsmInst> {
    let mut insts = Vec::new();
    let (addr, bank) = components(mgr, naming, ptr, &mut insts);
    let av = word_value(mgr, naming, addr, &mut insts);
    let bv = word_value(mgr, naming, bank, &mut insts);
    mgr.unpin_register(addr); mgr.unpin_register(bank);
    let product = naming.new_internal_temp();
    insts.extend(wide::lower_i32_binary(mgr, naming, IrBinaryOp::Mul, &bv, &Value::Constant(bank_size as i64), product));
    insts.extend(wide::lower_i32_binary(mgr, naming, IrBinaryOp::Add, &Value::Temp(product), &av, result));
    insts
}

/// General displacement path. All intermediate address calculations are 32-bit.
pub fn offset(mgr: &mut RegisterPressureManager, naming: &mut NameGenerator,
    ptr: &Value, index: &Value, size: u32, result: TempId, bank_size: u16) -> Vec<AsmInst> {
    assert_ne!(bank_size, 0, "bank size must be nonzero");
    if let Value::Constant(n) = index {
        return constant_offset(mgr, naming, ptr, n.wrapping_mul(size as i64), result, bank_size);
    }
    let base = naming.new_internal_temp();
    let mut insts = to_linear(mgr, naming, ptr, base, bank_size);
    let displacement = if size == 1 { index.clone() } else {
        let t = naming.new_internal_temp();
        insts.extend(wide::lower_i32_binary(mgr, naming, IrBinaryOp::Mul, index, &Value::Constant(size as i64), t));
        Value::Temp(t)
    };
    let total = naming.new_internal_temp();
    insts.extend(wide::lower_i32_binary(mgr, naming, IrBinaryOp::Add, &Value::Temp(base), &displacement, total));
    let bank = naming.new_internal_temp();
    let addr = naming.new_internal_temp();
    insts.extend(wide::lower_i32_binary(mgr, naming, IrBinaryOp::UDiv, &Value::Temp(total), &Value::Constant(bank_size as i64), bank));
    // Derive the remainder from the quotient; do not emit a second division.
    let used = naming.new_internal_temp();
    insts.extend(wide::lower_i32_binary(mgr, naming, IrBinaryOp::Mul, &Value::Temp(bank), &Value::Constant(bank_size as i64), used));
    insts.extend(wide::lower_i32_binary(mgr, naming, IrBinaryOp::Sub, &Value::Temp(total), &Value::Temp(used), addr));
    let bname = naming.temp_name(bank);
    let aname = naming.temp_name(addr);
    let ar = mgr.get_register(aname); insts.extend(mgr.take_instructions());
    mgr.bind_value_to_register(naming.temp_name(result), ar);
    mgr.set_pointer_bank(naming.temp_name(result), BankInfo::Dynamic(bname));
    insts
}

/// Constant increments need only native operations, including carry/borrow.
fn constant_offset(mgr: &mut RegisterPressureManager, naming: &mut NameGenerator,
    ptr: &Value, offset: i64, result: TempId, bank_size: u16) -> Vec<AsmInst> {
    let mut insts = vec![AsmInst::Comment("GEP: bank overflow checked before losing carry".into())];
    let (addr, bank) = components(mgr, naming, ptr, &mut insts);
    let quotient = offset.div_euclid(bank_size as i64);
    let remainder = offset.rem_euclid(bank_size as i64) as u16;
    let sum = reg(mgr, naming, &mut insts, "sum"); mgr.pin_register(sum);
    let carry = reg(mgr, naming, &mut insts, "carry"); mgr.pin_register(carry);
    let bs = reg(mgr, naming, &mut insts, "bank_size"); mgr.pin_register(bs);
    let bank_name = naming.temp_with_context("pointer", "offset_bank");
    let br = mgr.get_register(bank_name.clone()); insts.extend(mgr.take_instructions()); mgr.pin_register(br);
    insts.push(AsmInst::AddI(br, bank, quotient as i16));
    insts.push(AsmInst::AddI(sum, addr, remainder as i16));
    insts.push(AsmInst::Sltu(carry, sum, addr));
    insts.push(AsmInst::Li(bs, bank_size as i16));
    let adjust = naming.i32_label("gep_adjust");
    let done = naming.i32_label("gep_done");
    insts.push(AsmInst::Bne(carry, Reg::R0, adjust.clone()));
    insts.push(AsmInst::Sltu(carry, sum, bs));
    insts.push(AsmInst::Bne(carry, Reg::R0, done.clone()));
    insts.push(AsmInst::Label(adjust));
    insts.push(AsmInst::Sub(sum, sum, bs));
    insts.push(AsmInst::AddI(br, br, 1));
    insts.push(AsmInst::Label(done));
    mgr.bind_value_to_register(naming.temp_name(result), sum);
    mgr.set_pointer_bank(naming.temp_name(result), BankInfo::Dynamic(bank_name));
    for r in [addr, bank, sum, carry, bs, br] { mgr.unpin_register(r); }
    insts
}
