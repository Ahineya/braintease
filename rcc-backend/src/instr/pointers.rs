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
    if size == 0 {
        return constant_offset(mgr, naming, ptr, 0, result, bank_size);
    }
    if size == 1 && matches!(index, Value::Temp(t)
        if mgr.get_i32_high(&naming.temp_name(*t)).is_none()
            && mgr.get_i64_words(&naming.temp_name(*t)).is_none()) {
        return word_offset(mgr, naming, ptr, index, result, bank_size);
    }

    let mut insts = vec![AsmInst::Comment("GEP: native narrow displacement, wide fallback".into())];
    let (addr, bank) = components(mgr, naming, ptr, &mut insts);
    let (index_lo, index_hi, ii) = wide::get_i32_pair(mgr, naming, index, false);
    insts.extend(ii);
    mgr.pin_register(index_lo);
    mgr.pin_register(index_hi);

    // Allocate every working register before emitting internal branches. The
    // register allocator is linear: a spill emitted on just one arm would not
    // initialize its slot on the other arm. Work on owned copies and keep the
    // complete operation pinned so either arm has the same allocator state.
    let mut scratch = Vec::new();
    for tag in ["index_lo", "index_hi", "remainder_lo", "remainder_hi", "bits", "one", "carry", "temporary", "bank_size"] {
        let r = reg(mgr, naming, &mut insts, tag);
        mgr.pin_register(r);
        scratch.push(r);
        // Release the input pins as soon as both words have been copied; the
        // full operation uses eleven of the twelve allocatable registers.
        match scratch.len() {
            1 => insts.push(AsmInst::Move(r, index_lo)),
            2 => {
                insts.push(AsmInst::Move(r, index_hi));
                mgr.unpin_register(index_lo);
                mgr.unpin_register(index_hi);
            }
            _ => {}
        }
    }
    let (lo, hi, rlo, rhi, bits, one, carry, tmp, bs) =
        (scratch[0], scratch[1], scratch[2], scratch[3], scratch[4], scratch[5], scratch[6], scratch[7], scratch[8]);

    // Scale using shifts/adds in the existing registers. In particular, arrays
    // of 32-bit words use a two-word shift rather than a general multiplication.
    if size.is_power_of_two() {
        let shift = size.trailing_zeros();
        if shift >= 16 {
            insts.push(AsmInst::Li(bits, (shift - 16) as i16));
            insts.push(AsmInst::Sll(hi, lo, bits));
            insts.push(AsmInst::Li(lo, 0));
        } else if shift != 0 {
            insts.push(AsmInst::Li(bits, (16 - shift) as i16));
            insts.push(AsmInst::Srl(tmp, lo, bits));
            insts.push(AsmInst::Li(bits, shift as i16));
            insts.push(AsmInst::Sll(hi, hi, bits));
            insts.push(AsmInst::Or(hi, hi, tmp));
            insts.push(AsmInst::Sll(lo, lo, bits));
        }
    } else {
        insts.push(AsmInst::Li(rlo, 0));
        insts.push(AsmInst::Li(rhi, 0));
        for shift in 0..32 {
            if size & (1u32 << shift) == 0 { continue; }
            if shift >= 16 {
                insts.push(AsmInst::Li(bits, (shift - 16) as i16));
                insts.push(AsmInst::Sll(tmp, lo, bits));
                insts.push(AsmInst::Add(rhi, rhi, tmp));
                continue;
            }
            if shift == 0 {
                insts.push(AsmInst::Move(tmp, lo));
                insts.push(AsmInst::Move(one, hi));
            } else {
                insts.push(AsmInst::Li(bits, shift as i16));
                insts.push(AsmInst::Sll(tmp, lo, bits));
                insts.push(AsmInst::Sll(one, hi, bits));
                insts.push(AsmInst::Li(bits, (16 - shift) as i16));
                insts.push(AsmInst::Srl(bs, lo, bits));
                insts.push(AsmInst::Or(one, one, bs));
            }
            insts.push(AsmInst::Add(bs, rlo, tmp));
            insts.push(AsmInst::Sltu(carry, bs, rlo));
            insts.push(AsmInst::Move(rlo, bs));
            insts.push(AsmInst::Add(rhi, rhi, one));
            insts.push(AsmInst::Add(rhi, rhi, carry));
        }
        insts.push(AsmInst::Move(lo, rlo));
        insts.push(AsmInst::Move(hi, rhi));
    }

    let positive = naming.i32_label("gep_positive_word");
    let negative = naming.i32_label("gep_negative_word");
    let add = naming.i32_label("gep_native_add");
    let adjust = naming.i32_label("gep_native_carry");
    let subtract = naming.i32_label("gep_native_subtract");
    let general = naming.i32_label("gep_wide_offset");
    let divide = naming.i32_label("gep_divide");
    let ge = naming.i32_label("gep_divide_ge");
    let next = naming.i32_label("gep_divide_next");
    let finish_divide = naming.i32_label("gep_divide_done");
    let done = naming.i32_label("gep_offset_done");
    insts.push(AsmInst::Li(bs, bank_size as i16));
    // Integer-to-pointer casts can supply a noncanonical offset. Normalize
    // those using the full linear-address path before using single-carry math.
    insts.push(AsmInst::Sltu(tmp, addr, bs));
    insts.push(AsmInst::Beq(tmp, Reg::R0, general.clone()));
    insts.push(AsmInst::Beq(hi, Reg::R0, positive.clone()));
    insts.push(AsmInst::Li(tmp, -1));
    insts.push(AsmInst::Beq(hi, tmp, negative.clone()));
    insts.push(AsmInst::Beq(Reg::R0, Reg::R0, general.clone()));

    // An unsigned one-word displacement needs at most one carry adjustment
    // after removing its whole-bank portion. For the common index < bank_size
    // case this does not execute any division, even native 16-bit division.
    insts.push(AsmInst::Label(positive));
    insts.push(AsmInst::Sltu(tmp, lo, bs));
    insts.push(AsmInst::Bne(tmp, Reg::R0, add.clone()));
    insts.push(AsmInst::Div(tmp, lo, bs));
    insts.push(AsmInst::Add(bank, bank, tmp));
    insts.push(AsmInst::Mod(lo, lo, bs));
    insts.push(AsmInst::Label(add));
    insts.push(AsmInst::Add(tmp, addr, lo));
    insts.push(AsmInst::Sltu(carry, tmp, addr));
    insts.push(AsmInst::Move(addr, tmp));
    insts.push(AsmInst::Bne(carry, Reg::R0, adjust.clone()));
    insts.push(AsmInst::Sltu(tmp, addr, bs));
    insts.push(AsmInst::Bne(tmp, Reg::R0, done.clone()));
    insts.push(AsmInst::Label(adjust));
    insts.push(AsmInst::Sub(addr, addr, bs));
    insts.push(AsmInst::AddI(bank, bank, 1));
    insts.push(AsmInst::Beq(Reg::R0, Reg::R0, done.clone()));

    insts.push(AsmInst::Label(negative));
    // -65536 has no one-word magnitude. Also keep unsigned 32-bit wraparound
    // semantics for underflowing linear addresses by using the general path
    // when the base might be smaller than the displacement magnitude.
    insts.push(AsmInst::Beq(lo, Reg::R0, general.clone()));
    insts.push(AsmInst::Li(tmp, (u16::MAX as u32).div_ceil(bank_size as u32) as i16));
    insts.push(AsmInst::Sltu(tmp, bank, tmp));
    insts.push(AsmInst::Bne(tmp, Reg::R0, general.clone()));
    insts.push(AsmInst::Sub(lo, Reg::R0, lo));
    insts.push(AsmInst::Sltu(tmp, lo, bs));
    insts.push(AsmInst::Bne(tmp, Reg::R0, subtract.clone()));
    insts.push(AsmInst::Div(tmp, lo, bs));
    insts.push(AsmInst::Sub(bank, bank, tmp));
    insts.push(AsmInst::Mod(lo, lo, bs));
    insts.push(AsmInst::Label(subtract));
    insts.push(AsmInst::Sltu(carry, addr, lo));
    insts.push(AsmInst::Sub(addr, addr, lo));
    insts.push(AsmInst::Beq(carry, Reg::R0, done.clone()));
    insts.push(AsmInst::Add(addr, addr, bs));
    insts.push(AsmInst::AddI(bank, bank, -1));
    insts.push(AsmInst::Beq(Reg::R0, Reg::R0, done.clone()));

    // Full fallback: form (bank * bank_size + addr + displacement) mod 2^32,
    // then divide by the one-word bank size. This retains the previous behavior
    // for arbitrary signed offsets, including linear-address wraparound.
    insts.push(AsmInst::Label(general));
    insts.push(AsmInst::Add(tmp, lo, addr));
    insts.push(AsmInst::Sltu(carry, tmp, lo));
    insts.push(AsmInst::Move(lo, tmp));
    insts.push(AsmInst::Add(hi, hi, carry));
    for shift in 0..16 {
        if bank_size & (1u16 << shift) == 0 { continue; }
        if shift == 0 {
            insts.push(AsmInst::Move(rlo, bank));
            insts.push(AsmInst::Li(rhi, 0));
        } else {
            insts.push(AsmInst::Li(bits, shift as i16));
            insts.push(AsmInst::Sll(rlo, bank, bits));
            insts.push(AsmInst::Li(bits, (16 - shift) as i16));
            insts.push(AsmInst::Srl(rhi, bank, bits));
        }
        insts.push(AsmInst::Add(tmp, lo, rlo));
        insts.push(AsmInst::Sltu(carry, tmp, lo));
        insts.push(AsmInst::Move(lo, tmp));
        insts.push(AsmInst::Add(hi, hi, rhi));
        insts.push(AsmInst::Add(hi, hi, carry));
    }
    insts.push(AsmInst::Li(addr, 0));
    insts.push(AsmInst::Li(rlo, 0));
    insts.push(AsmInst::Li(rhi, 0));
    insts.push(AsmInst::Li(bits, 32));
    insts.push(AsmInst::Li(one, 1));
    insts.push(AsmInst::Label(divide.clone()));
    insts.push(AsmInst::Slt(tmp, hi, Reg::R0));
    insts.push(AsmInst::Slt(carry, rlo, Reg::R0));
    insts.push(AsmInst::Add(rlo, rlo, rlo));
    insts.push(AsmInst::Or(rlo, rlo, tmp));
    insts.push(AsmInst::Add(rhi, rhi, rhi));
    insts.push(AsmInst::Or(rhi, rhi, carry));
    insts.push(AsmInst::Slt(carry, lo, Reg::R0));
    insts.push(AsmInst::Add(lo, lo, lo));
    insts.push(AsmInst::Add(hi, hi, hi));
    insts.push(AsmInst::Or(hi, hi, carry));
    // Only the low quotient word is part of the pointer's bank component.
    insts.push(AsmInst::Add(addr, addr, addr));
    insts.push(AsmInst::Bne(rhi, Reg::R0, ge.clone()));
    insts.push(AsmInst::Sltu(carry, rlo, bs));
    insts.push(AsmInst::Bne(carry, Reg::R0, next.clone()));
    insts.push(AsmInst::Label(ge));
    insts.push(AsmInst::Sltu(carry, rlo, bs));
    insts.push(AsmInst::Sub(rlo, rlo, bs));
    insts.push(AsmInst::Sub(rhi, rhi, carry));
    insts.push(AsmInst::Or(addr, addr, one));
    insts.push(AsmInst::Label(next));
    insts.push(AsmInst::Sub(bits, bits, one));
    insts.push(AsmInst::Beq(bits, Reg::R0, finish_divide.clone()));
    insts.push(AsmInst::Beq(Reg::R0, Reg::R0, divide));
    insts.push(AsmInst::Label(finish_divide));
    insts.push(AsmInst::Move(bank, addr));
    insts.push(AsmInst::Move(addr, rlo));
    insts.push(AsmInst::Label(done));

    let bank_name = naming.temp_with_context("pointer", "offset_bank");
    mgr.bind_value_to_register(bank_name.clone(), bank);
    mgr.bind_value_to_register(naming.temp_name(result), addr);
    mgr.set_pointer_bank(naming.temp_name(result), BankInfo::Dynamic(bank_name));
    mgr.unpin_register(addr);
    mgr.unpin_register(bank);
    for r in scratch {
        mgr.unpin_register(r);
        mgr.free_register(r);
    }
    insts
}

/// A zero-extended native index cannot overflow the 32-bit linear address:
/// bank * bank_size + offset + index <= 65535 * 65535 + 2 * 65535.
/// Normalize each native word first, then add with one bank carry. This also
/// accepts noncanonical offsets supplied by integer-to-pointer casts, without
/// reserving the nine scratch registers needed by the general wide fallback.
fn word_offset(mgr: &mut RegisterPressureManager, naming: &mut NameGenerator,
    ptr: &Value, index: &Value, result: TempId, bank_size: u16) -> Vec<AsmInst> {
    let mut insts = vec![AsmInst::Comment("GEP: zero-extended native displacement".into())];
    let (addr, bank) = components(mgr, naming, ptr, &mut insts);
    let input = mgr.get_value_register(index);
    insts.extend(mgr.take_instructions());
    mgr.pin_register(input);
    let delta = reg(mgr, naming, &mut insts, "word_delta");
    mgr.pin_register(delta);
    insts.push(AsmInst::Move(delta, input));
    mgr.unpin_register(input);
    let bs = reg(mgr, naming, &mut insts, "word_bank_size");
    mgr.pin_register(bs);
    let tmp = reg(mgr, naming, &mut insts, "word_temporary");
    mgr.pin_register(tmp);
    let carry = reg(mgr, naming, &mut insts, "word_carry");
    mgr.pin_register(carry);
    // All allocations precede branches, so no spill slot is initialized on
    // only one path through this instruction.
    insts.push(AsmInst::Li(bs, bank_size as i16));
    for word in [addr, delta] {
        let normalized = naming.i32_label("gep_word_normalized");
        insts.push(AsmInst::Sltu(tmp, word, bs));
        insts.push(AsmInst::Bne(tmp, Reg::R0, normalized.clone()));
        insts.push(AsmInst::Div(tmp, word, bs));
        insts.push(AsmInst::Add(bank, bank, tmp));
        insts.push(AsmInst::Mod(word, word, bs));
        insts.push(AsmInst::Label(normalized));
    }
    insts.push(AsmInst::Add(tmp, addr, delta));
    insts.push(AsmInst::Sltu(carry, tmp, addr));
    insts.push(AsmInst::Move(addr, tmp));
    let adjust = naming.i32_label("gep_word_adjust");
    let done = naming.i32_label("gep_word_done");
    insts.push(AsmInst::Bne(carry, Reg::R0, adjust.clone()));
    insts.push(AsmInst::Sltu(tmp, addr, bs));
    insts.push(AsmInst::Bne(tmp, Reg::R0, done.clone()));
    insts.push(AsmInst::Label(adjust));
    insts.push(AsmInst::Sub(addr, addr, bs));
    insts.push(AsmInst::AddI(bank, bank, 1));
    insts.push(AsmInst::Label(done));
    let bank_name = naming.temp_with_context("pointer", "word_offset_bank");
    mgr.bind_value_to_register(bank_name.clone(), bank);
    mgr.bind_value_to_register(naming.temp_name(result), addr);
    mgr.set_pointer_bank(naming.temp_name(result), BankInfo::Dynamic(bank_name));
    mgr.unpin_register(addr);
    mgr.unpin_register(bank);
    for r in [delta, bs, tmp, carry] {
        mgr.unpin_register(r);
        mgr.free_register(r);
    }
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
