//! 32-bit (I32 / `long`) lowering: two 16-bit words, little-endian.
//!
//! Low word in the named temp, high word in `{name}__hi`. Carry for add/sub is
//! synthesized with SLTU (Ripple has no ADC). 16-bit MUL/SLL/SRL only produce
//! or shift 16 bits (SLL/SRL mask the count to 0–15).

use rcc_frontend::ir::{IrBinaryOp, IrUnaryOp, Value};
use rcc_common::TempId;
use crate::regmgmt::RegisterPressureManager;
use crate::naming::NameGenerator;
use rcc_codegen::{AsmInst, Reg};
use log::debug;

pub fn split_const(c: i64) -> (i16, i16) {
    let u = c as u32;
    ((u & 0xFFFF) as i16, ((u >> 16) & 0xFFFF) as i16)
}

fn const_fits_i16(c: i64) -> bool {
    c >= i16::MIN as i64 && c <= i16::MAX as i64
}

/// True when this value must be treated as a 32-bit pair.
pub fn value_is_i32(
    mgr: &RegisterPressureManager,
    naming: &NameGenerator,
    value: &Value,
) -> bool {
    match value {
        Value::Temp(t) => mgr.get_i32_high(&naming.temp_name(*t)).is_some(),
        Value::Constant(c) => !const_fits_i16(*c),
        _ => false,
    }
}

pub fn bind_i32(
    mgr: &mut RegisterPressureManager,
    naming: &NameGenerator,
    lo_name: &str,
    lo_reg: Reg,
    hi_reg: Reg,
) {
    let hi_name = naming.i32_high_name(lo_name);
    mgr.bind_value_to_register(lo_name.to_string(), lo_reg);
    mgr.bind_value_to_register(hi_name.clone(), hi_reg);
    mgr.set_i32_high(lo_name.to_string(), hi_name);
}

/// Materialize `(lo, hi)` for an I32 operand. Narrow temps are sign- or zero-extended.
pub fn get_i32_pair(
    mgr: &mut RegisterPressureManager,
    naming: &mut NameGenerator,
    value: &Value,
    sign_extend: bool,
) -> (Reg, Reg, Vec<AsmInst>) {
    let mut insts = Vec::new();
    match value {
        Value::Temp(t) => {
            let lo_name = naming.temp_name(*t);
            let lo_reg = mgr.get_register(lo_name.clone());
            insts.extend(mgr.take_instructions());
            if let Some(hi_name) = mgr.get_i32_high(&lo_name) {
                let hi_reg = mgr.get_register(hi_name);
                insts.extend(mgr.take_instructions());
                (lo_reg, hi_reg, insts)
            } else {
                mgr.pin_register(lo_reg);
                let (hi_reg, ext) = extend_i16(mgr, naming, lo_reg, sign_extend);
                insts.extend(ext);
                mgr.unpin_register(lo_reg);
                (lo_reg, hi_reg, insts)
            }
        }
        Value::Constant(c) => {
            let (lo, hi) = split_const(*c);
            let lo_name = naming.const_value(*c);
            let lo_reg = mgr.get_register(lo_name);
            insts.extend(mgr.take_instructions());
            insts.push(AsmInst::Li(lo_reg, lo));
            mgr.pin_register(lo_reg);
            let hi_name = naming.temp_with_context("i32", "const_hi");
            let hi_reg = mgr.get_register(hi_name);
            insts.extend(mgr.take_instructions());
            insts.push(AsmInst::Li(hi_reg, hi));
            mgr.unpin_register(lo_reg);
            (lo_reg, hi_reg, insts)
        }
        _ => panic!("I32 operand must be a temp or constant, got {value:?}"),
    }
}

fn extend_i16(
    mgr: &mut RegisterPressureManager,
    naming: &mut NameGenerator,
    src: Reg,
    sign_extend: bool,
) -> (Reg, Vec<AsmInst>) {
    let mut insts = Vec::new();
    let hi_name = naming.temp_with_context("i32", if sign_extend { "sext" } else { "zext" });
    let hi = mgr.get_register(hi_name);
    insts.extend(mgr.take_instructions());
    mgr.pin_register(hi);
    if sign_extend {
        // Arithmetic shift of the 16-bit value by 15: 0 or 0xFFFF
        let sh = mgr.get_register(naming.temp_with_context("i32", "sh15"));
        insts.extend(mgr.take_instructions());
        insts.push(AsmInst::Li(sh, 15));
        insts.push(AsmInst::Srl(hi, src, sh)); // logical: 0 or 1
        // bit is 0 or 1. We want 0 or 0xFFFF. 0 - bit works: 0 or -1.
        insts.push(AsmInst::Sub(hi, Reg::R0, hi));
        mgr.free_register(sh);
    } else {
        insts.push(AsmInst::Li(hi, 0));
    }
    mgr.unpin_register(hi);
    (hi, insts)
}

/// Operand for a single instruction: a caller-pinned physical register, or a
/// named temp that is reloaded (and pinned only for this instruction).
enum Src<'a> {
    R(Reg),
    N(&'a str),
}

fn materialize(
    mgr: &mut RegisterPressureManager,
    insts: &mut Vec<AsmInst>,
    src: Src<'_>,
    unpin: &mut Vec<Reg>,
) -> Reg {
    match src {
        Src::R(r) => r,
        Src::N(n) => {
            let r = mgr.get_register(n.to_string());
            insts.extend(mgr.take_instructions());
            mgr.pin_register(r);
            unpin.push(r);
            r
        }
    }
}

fn finish_pins(mgr: &mut RegisterPressureManager, unpin: Vec<Reg>) {
    for r in unpin {
        mgr.unpin_register(r);
    }
}

fn new_temp(
    mgr: &mut RegisterPressureManager,
    naming: &mut NameGenerator,
    insts: &mut Vec<AsmInst>,
    ctx: &str,
    suffix: &str,
) -> String {
    let name = naming.temp_with_context(ctx, suffix);
    let _ = mgr.get_register(name.clone());
    insts.extend(mgr.take_instructions());
    name
}

fn em1(
    mgr: &mut RegisterPressureManager,
    insts: &mut Vec<AsmInst>,
    a: Src<'_>,
    f: impl FnOnce(Reg) -> AsmInst,
) {
    let mut unpin = Vec::new();
    let ra = materialize(mgr, insts, a, &mut unpin);
    insts.push(f(ra));
    finish_pins(mgr, unpin);
}

fn em2(
    mgr: &mut RegisterPressureManager,
    insts: &mut Vec<AsmInst>,
    a: Src<'_>,
    b: Src<'_>,
    f: impl FnOnce(Reg, Reg) -> AsmInst,
) {
    let mut unpin = Vec::new();
    let ra = materialize(mgr, insts, a, &mut unpin);
    let rb = materialize(mgr, insts, b, &mut unpin);
    insts.push(f(ra, rb));
    finish_pins(mgr, unpin);
}

fn em3(
    mgr: &mut RegisterPressureManager,
    insts: &mut Vec<AsmInst>,
    d: Src<'_>,
    a: Src<'_>,
    b: Src<'_>,
    f: impl FnOnce(Reg, Reg, Reg) -> AsmInst,
) {
    let mut unpin = Vec::new();
    let rd = materialize(mgr, insts, d, &mut unpin);
    let ra = materialize(mgr, insts, a, &mut unpin);
    let rb = materialize(mgr, insts, b, &mut unpin);
    insts.push(f(rd, ra, rb));
    finish_pins(mgr, unpin);
}

fn em_li(mgr: &mut RegisterPressureManager, insts: &mut Vec<AsmInst>, dst: Src<'_>, imm: i16) {
    em1(mgr, insts, dst, |r| AsmInst::Li(r, imm));
}

fn emit_i32_add(
    result_lo: Reg,
    result_hi: Reg,
    a_lo: Reg,
    a_hi: Reg,
    b_lo: Reg,
    b_hi: Reg,
    mgr: &mut RegisterPressureManager,
    naming: &mut NameGenerator,
) -> Vec<AsmInst> {
    let mut insts = Vec::new();
    let carry = new_temp(mgr, naming, &mut insts, "i32", "carry");
    // Carry is (a_lo+b_lo) <u a_lo. If dest aliases a_lo, compare against b_lo
    // (valid unless dest also aliases b_lo, i.e. a+a into a).
    let orig_a = if result_lo == a_lo {
        let n = new_temp(mgr, naming, &mut insts, "i32", "add_orig_a");
        em3(mgr, &mut insts, Src::N(&n), Src::R(a_lo), Src::R(Reg::R0), AsmInst::Add);
        Some(n)
    } else {
        None
    };
    insts.push(AsmInst::Add(result_lo, a_lo, b_lo));
    if let Some(ref n) = orig_a {
        em3(mgr, &mut insts, Src::N(&carry), Src::R(result_lo), Src::N(n), AsmInst::Sltu);
    } else {
        em3(mgr, &mut insts, Src::N(&carry), Src::R(result_lo), Src::R(a_lo), AsmInst::Sltu);
    }
    insts.push(AsmInst::Add(result_hi, a_hi, b_hi));
    em3(mgr, &mut insts, Src::R(result_hi), Src::R(result_hi), Src::N(&carry), AsmInst::Add);
    insts
}

fn emit_i32_sub(
    result_lo: Reg,
    result_hi: Reg,
    a_lo: Reg,
    a_hi: Reg,
    b_lo: Reg,
    b_hi: Reg,
    mgr: &mut RegisterPressureManager,
    naming: &mut NameGenerator,
) -> Vec<AsmInst> {
    let mut insts = Vec::new();
    let borrow = new_temp(mgr, naming, &mut insts, "i32", "borrow");
    // Borrow must be computed before dest can clobber a_lo (in-place sub).
    em3(mgr, &mut insts, Src::N(&borrow), Src::R(a_lo), Src::R(b_lo), AsmInst::Sltu);
    insts.push(AsmInst::Sub(result_lo, a_lo, b_lo));
    insts.push(AsmInst::Sub(result_hi, a_hi, b_hi));
    em3(mgr, &mut insts, Src::R(result_hi), Src::R(result_hi), Src::N(&borrow), AsmInst::Sub);
    insts
}

/// Unsigned 16×16 → 32 using 8-bit partial products (MUL is 16×16→16).
pub(crate) fn emit_umul16x16(
    dst_lo: Reg,
    dst_hi: Reg,
    a: Reg,
    b: Reg,
    mgr: &mut RegisterPressureManager,
    naming: &mut NameGenerator,
) -> Vec<AsmInst> {
    let mut insts = Vec::new();
    let mask = new_temp(mgr, naming, &mut insts, "mul", "mask");
    em_li(mgr, &mut insts, Src::N(&mask), 0xFF);
    let sh8 = new_temp(mgr, naming, &mut insts, "mul", "sh8");
    em_li(mgr, &mut insts, Src::N(&sh8), 8);

    let a0 = new_temp(mgr, naming, &mut insts, "mul", "a0");
    em3(mgr, &mut insts, Src::N(&a0), Src::R(a), Src::N(&mask), AsmInst::And);
    let a1 = new_temp(mgr, naming, &mut insts, "mul", "a1");
    em3(mgr, &mut insts, Src::N(&a1), Src::R(a), Src::N(&sh8), AsmInst::Srl);
    let b0 = new_temp(mgr, naming, &mut insts, "mul", "b0");
    em3(mgr, &mut insts, Src::N(&b0), Src::R(b), Src::N(&mask), AsmInst::And);
    let b1 = new_temp(mgr, naming, &mut insts, "mul", "b1");
    em3(mgr, &mut insts, Src::N(&b1), Src::R(b), Src::N(&sh8), AsmInst::Srl);

    let p00 = new_temp(mgr, naming, &mut insts, "mul", "p00");
    em3(mgr, &mut insts, Src::N(&p00), Src::N(&a0), Src::N(&b0), AsmInst::Mul);
    let p01 = new_temp(mgr, naming, &mut insts, "mul", "p01");
    em3(mgr, &mut insts, Src::N(&p01), Src::N(&a0), Src::N(&b1), AsmInst::Mul);
    let p10 = new_temp(mgr, naming, &mut insts, "mul", "p10");
    em3(mgr, &mut insts, Src::N(&p10), Src::N(&a1), Src::N(&b0), AsmInst::Mul);
    em3(mgr, &mut insts, Src::R(dst_hi), Src::N(&a1), Src::N(&b1), AsmInst::Mul);

    let mid = new_temp(mgr, naming, &mut insts, "mul", "mid");
    em3(mgr, &mut insts, Src::N(&mid), Src::N(&p01), Src::N(&p10), AsmInst::Add);
    let mid_c = new_temp(mgr, naming, &mut insts, "mul", "midc");
    em3(mgr, &mut insts, Src::N(&mid_c), Src::N(&mid), Src::N(&p01), AsmInst::Sltu);

    let mid_lo = new_temp(mgr, naming, &mut insts, "mul", "midlo");
    em3(mgr, &mut insts, Src::N(&mid_lo), Src::N(&mid), Src::N(&mask), AsmInst::And);
    em3(mgr, &mut insts, Src::N(&mid_lo), Src::N(&mid_lo), Src::N(&sh8), AsmInst::Sll);
    em3(mgr, &mut insts, Src::R(dst_lo), Src::N(&p00), Src::N(&mid_lo), AsmInst::Add);
    let c = new_temp(mgr, naming, &mut insts, "mul", "c");
    em3(mgr, &mut insts, Src::N(&c), Src::R(dst_lo), Src::N(&p00), AsmInst::Sltu);

    em3(mgr, &mut insts, Src::N(&mid), Src::N(&mid), Src::N(&sh8), AsmInst::Srl);
    em3(mgr, &mut insts, Src::R(dst_hi), Src::R(dst_hi), Src::N(&mid), AsmInst::Add);
    em3(mgr, &mut insts, Src::N(&mid_c), Src::N(&mid_c), Src::N(&sh8), AsmInst::Sll);
    em3(mgr, &mut insts, Src::R(dst_hi), Src::R(dst_hi), Src::N(&mid_c), AsmInst::Add);
    em3(mgr, &mut insts, Src::R(dst_hi), Src::R(dst_hi), Src::N(&c), AsmInst::Add);
    insts
}

fn emit_i32_mul(
    result_lo: Reg,
    result_hi: Reg,
    a_lo: Reg,
    a_hi: Reg,
    b_lo: Reg,
    b_hi: Reg,
    mgr: &mut RegisterPressureManager,
    naming: &mut NameGenerator,
) -> Vec<AsmInst> {
    let mut insts = Vec::new();
    insts.extend(emit_umul16x16(result_lo, result_hi, a_lo, b_lo, mgr, naming));
    let cross1 = new_temp(mgr, naming, &mut insts, "mul32", "x1");
    em3(mgr, &mut insts, Src::N(&cross1), Src::R(a_lo), Src::R(b_hi), AsmInst::Mul);
    let cross2 = new_temp(mgr, naming, &mut insts, "mul32", "x2");
    em3(mgr, &mut insts, Src::N(&cross2), Src::R(a_hi), Src::R(b_lo), AsmInst::Mul);
    em3(mgr, &mut insts, Src::R(result_hi), Src::R(result_hi), Src::N(&cross1), AsmInst::Add);
    em3(mgr, &mut insts, Src::R(result_hi), Src::R(result_hi), Src::N(&cross2), AsmInst::Add);
    insts
}

fn emit_i32_ult(
    result: Reg,
    a_lo: Reg,
    a_hi: Reg,
    b_lo: Reg,
    b_hi: Reg,
    _mgr: &mut RegisterPressureManager,
    naming: &mut NameGenerator,
) -> Vec<AsmInst> {
    // Branching form uses only `result`, so it is safe when 11 registers are already pinned.
    let mut insts = Vec::new();
    let hi_ne = naming.i32_label("ult_hine");
    let done = naming.i32_label("ult_done");
    insts.push(AsmInst::Xor(result, a_hi, b_hi));
    insts.push(AsmInst::Bne(result, Reg::R0, hi_ne.clone()));
    insts.push(AsmInst::Sltu(result, a_lo, b_lo));
    insts.push(AsmInst::Beq(Reg::R0, Reg::R0, done.clone()));
    insts.push(AsmInst::Label(hi_ne));
    insts.push(AsmInst::Sltu(result, a_hi, b_hi));
    insts.push(AsmInst::Label(done));
    insts
}

fn emit_i32_slt(
    result: Reg,
    a_lo: Reg,
    a_hi: Reg,
    b_lo: Reg,
    b_hi: Reg,
    _mgr: &mut RegisterPressureManager,
    naming: &mut NameGenerator,
) -> Vec<AsmInst> {
    let mut insts = Vec::new();
    let hi_ne = naming.i32_label("slt_hine");
    let done = naming.i32_label("slt_done");
    insts.push(AsmInst::Xor(result, a_hi, b_hi));
    insts.push(AsmInst::Bne(result, Reg::R0, hi_ne.clone()));
    insts.push(AsmInst::Sltu(result, a_lo, b_lo));
    insts.push(AsmInst::Beq(Reg::R0, Reg::R0, done.clone()));
    insts.push(AsmInst::Label(hi_ne));
    insts.push(AsmInst::Slt(result, a_hi, b_hi));
    insts.push(AsmInst::Label(done));
    insts
}

fn invert01(mgr: &mut RegisterPressureManager, naming: &mut NameGenerator, dst: Reg, src: Reg) -> Vec<AsmInst> {
    let mut insts = Vec::new();
    let one = new_temp(mgr, naming, &mut insts, "cmp", "inv1");
    em_li(mgr, &mut insts, Src::N(&one), 1);
    em3(mgr, &mut insts, Src::R(dst), Src::N(&one), Src::R(src), AsmInst::Sub);
    insts
}

/// 32-bit unsigned restoring division. `want_rem` selects remainder vs quotient.
///
/// Loop-carried values stay in pinned physical registers. The allocator is
/// linear and does not understand labels; named temps would go stale at merges.
fn emit_i32_udiv(
    result_lo: Reg,
    result_hi: Reg,
    n_lo: Reg,
    n_hi: Reg,
    d_lo: Reg,
    d_hi: Reg,
    want_rem: bool,
    mgr: &mut RegisterPressureManager,
    naming: &mut NameGenerator,
) -> Vec<AsmInst> {
    let mut insts = Vec::new();
    insts.push(AsmInst::Comment("i32 unsigned div/rem (restoring)".to_string()));

    fn alloc_pin(
        mgr: &mut RegisterPressureManager,
        naming: &mut NameGenerator,
        insts: &mut Vec<AsmInst>,
        ctx: &str,
        suffix: &str,
    ) -> Reg {
        let name = naming.temp_with_context(ctx, suffix);
        let r = mgr.get_register(name);
        insts.extend(mgr.take_instructions());
        mgr.pin_register(r);
        r
    }

    let wn_lo = alloc_pin(mgr, naming, &mut insts, "div", "nlo");
    insts.push(AsmInst::Add(wn_lo, n_lo, Reg::R0));
    let wn_hi = alloc_pin(mgr, naming, &mut insts, "div", "nhi");
    insts.push(AsmInst::Add(wn_hi, n_hi, Reg::R0));
    mgr.unpin_register(n_lo);
    mgr.unpin_register(n_hi);

    let r_lo = alloc_pin(mgr, naming, &mut insts, "div", "rlo");
    insts.push(AsmInst::Li(r_lo, 0));
    let r_hi = alloc_pin(mgr, naming, &mut insts, "div", "rhi");
    insts.push(AsmInst::Li(r_hi, 0));

    insts.push(AsmInst::Li(result_lo, 0));
    insts.push(AsmInst::Li(result_hi, 0));

    let bits = alloc_pin(mgr, naming, &mut insts, "div", "bits");
    insts.push(AsmInst::Li(bits, 32));
    let one = alloc_pin(mgr, naming, &mut insts, "div", "one");
    insts.push(AsmInst::Li(one, 1));
    let scratch = alloc_pin(mgr, naming, &mut insts, "div", "scr");
    let scratch2 = alloc_pin(mgr, naming, &mut insts, "div", "scr2");

    let loop_l = naming.i32_label("udiv_loop");
    let ge_l = naming.i32_label("udiv_ge");
    let next_l = naming.i32_label("udiv_next");
    let done_l = naming.i32_label("udiv_done");

    insts.push(AsmInst::Label(loop_l.clone()));

    // R <<= 1; R |= n_msb. <<1 is ADD x,x,x so we do not need a shift-count reg.
    insts.push(AsmInst::Slt(scratch, wn_hi, Reg::R0)); // n bit 31
    insts.push(AsmInst::Slt(scratch2, r_lo, Reg::R0)); // r bit 15
    insts.push(AsmInst::Add(r_lo, r_lo, r_lo));
    insts.push(AsmInst::Or(r_lo, r_lo, scratch));
    insts.push(AsmInst::Add(r_hi, r_hi, r_hi));
    insts.push(AsmInst::Or(r_hi, r_hi, scratch2));

    insts.push(AsmInst::Slt(scratch2, wn_lo, Reg::R0));
    insts.push(AsmInst::Add(wn_lo, wn_lo, wn_lo));
    insts.push(AsmInst::Add(wn_hi, wn_hi, wn_hi));
    insts.push(AsmInst::Or(wn_hi, wn_hi, scratch2));

    insts.push(AsmInst::Slt(scratch2, result_lo, Reg::R0));
    insts.push(AsmInst::Add(result_lo, result_lo, result_lo));
    insts.push(AsmInst::Add(result_hi, result_hi, result_hi));
    insts.push(AsmInst::Or(result_hi, result_hi, scratch2));

    insts.extend(emit_i32_ult(scratch, r_lo, r_hi, d_lo, d_hi, mgr, naming));
    insts.push(AsmInst::Bne(scratch, Reg::R0, next_l.clone()));
    insts.push(AsmInst::Beq(Reg::R0, Reg::R0, ge_l.clone()));

    insts.push(AsmInst::Label(ge_l));
    insts.push(AsmInst::Sltu(scratch, r_lo, d_lo));
    insts.push(AsmInst::Sub(r_lo, r_lo, d_lo));
    insts.push(AsmInst::Sub(r_hi, r_hi, d_hi));
    insts.push(AsmInst::Sub(r_hi, r_hi, scratch));
    insts.push(AsmInst::Or(result_lo, result_lo, one));

    insts.push(AsmInst::Label(next_l));
    insts.push(AsmInst::Sub(bits, bits, one));
    insts.push(AsmInst::Beq(bits, Reg::R0, done_l.clone()));
    insts.push(AsmInst::Beq(Reg::R0, Reg::R0, loop_l));

    insts.push(AsmInst::Label(done_l));
    if want_rem {
        insts.push(AsmInst::Add(result_lo, r_lo, Reg::R0));
        insts.push(AsmInst::Add(result_hi, r_hi, Reg::R0));
    }

    for r in [wn_lo, wn_hi, r_lo, r_hi, bits, one, scratch, scratch2] {
        mgr.unpin_register(r);
    }
    insts
}

/// Two's complement negate. `scratch` must be distinct from `dst_*` and `src_*`.
fn emit_i32_neg(
    dst_lo: Reg,
    dst_hi: Reg,
    src_lo: Reg,
    src_hi: Reg,
    scratch: Reg,
) -> Vec<AsmInst> {
    vec![
        AsmInst::Sltu(scratch, Reg::R0, src_lo),
        AsmInst::Sub(dst_lo, Reg::R0, src_lo),
        AsmInst::Sub(dst_hi, Reg::R0, src_hi),
        AsmInst::Sub(dst_hi, dst_hi, scratch),
    ]
}

fn pin_named(
    mgr: &mut RegisterPressureManager,
    insts: &mut Vec<AsmInst>,
    name: &str,
) -> Reg {
    let r = mgr.get_register(name.to_string());
    insts.extend(mgr.take_instructions());
    mgr.pin_register(r);
    r
}

fn emit_i32_sdiv(
    result_lo: Reg,
    result_hi: Reg,
    n_lo: Reg,
    n_hi: Reg,
    d_lo: Reg,
    d_hi: Reg,
    want_rem: bool,
    mgr: &mut RegisterPressureManager,
    naming: &mut NameGenerator,
) -> Vec<AsmInst> {
    let mut insts = Vec::new();
    let nneg = new_temp(mgr, naming, &mut insts, "sdiv", "nneg");
    em3(mgr, &mut insts, Src::N(&nneg), Src::R(n_hi), Src::R(Reg::R0), AsmInst::Slt);
    let nneg_r = pin_named(mgr, &mut insts, &nneg);
    let dneg = new_temp(mgr, naming, &mut insts, "sdiv", "dneg");
    em3(mgr, &mut insts, Src::N(&dneg), Src::R(d_hi), Src::R(Reg::R0), AsmInst::Slt);
    let dneg_r = pin_named(mgr, &mut insts, &dneg);

    let an_lo = new_temp(mgr, naming, &mut insts, "sdiv", "anlo");
    let an_hi = new_temp(mgr, naming, &mut insts, "sdiv", "anhi");
    em3(mgr, &mut insts, Src::N(&an_lo), Src::R(n_lo), Src::R(Reg::R0), AsmInst::Add);
    let an_lo_r = pin_named(mgr, &mut insts, &an_lo);
    em3(mgr, &mut insts, Src::N(&an_hi), Src::R(n_hi), Src::R(Reg::R0), AsmInst::Add);
    let an_hi_r = pin_named(mgr, &mut insts, &an_hi);
    let nneg_l = naming.i32_label("sdiv_nneg");
    let npos_l = naming.i32_label("sdiv_npos");
    insts.push(AsmInst::Beq(nneg_r, Reg::R0, npos_l.clone()));
    insts.extend(emit_i32_neg(an_lo_r, an_hi_r, n_lo, n_hi, result_lo));
    insts.push(AsmInst::Beq(Reg::R0, Reg::R0, nneg_l.clone()));
    insts.push(AsmInst::Label(npos_l));
    insts.push(AsmInst::Label(nneg_l));
    mgr.unpin_register(n_lo);
    mgr.unpin_register(n_hi);

    let ad_lo = new_temp(mgr, naming, &mut insts, "sdiv", "adlo");
    let ad_hi = new_temp(mgr, naming, &mut insts, "sdiv", "adhi");
    em3(mgr, &mut insts, Src::N(&ad_lo), Src::R(d_lo), Src::R(Reg::R0), AsmInst::Add);
    let ad_lo_r = pin_named(mgr, &mut insts, &ad_lo);
    em3(mgr, &mut insts, Src::N(&ad_hi), Src::R(d_hi), Src::R(Reg::R0), AsmInst::Add);
    let ad_hi_r = pin_named(mgr, &mut insts, &ad_hi);
    let dneg_l = naming.i32_label("sdiv_dneg");
    let dpos_l = naming.i32_label("sdiv_dpos");
    insts.push(AsmInst::Beq(dneg_r, Reg::R0, dpos_l.clone()));
    insts.extend(emit_i32_neg(ad_lo_r, ad_hi_r, d_lo, d_hi, result_lo));
    insts.push(AsmInst::Beq(Reg::R0, Reg::R0, dneg_l.clone()));
    insts.push(AsmInst::Label(dpos_l));
    insts.push(AsmInst::Label(dneg_l));
    mgr.unpin_register(d_lo);
    mgr.unpin_register(d_hi);

    // Sign flags must not be spilled only on a skipped negate path. Unpin
    // them here so udiv may spill them in always-executed setup.
    mgr.unpin_register(nneg_r);
    mgr.unpin_register(dneg_r);

    insts.extend(emit_i32_udiv(
        result_lo, result_hi, an_lo_r, an_hi_r, ad_lo_r, ad_hi_r, want_rem, mgr, naming,
    ));
    mgr.unpin_register(an_lo_r);
    mgr.unpin_register(an_hi_r);
    mgr.unpin_register(ad_lo_r);
    mgr.unpin_register(ad_hi_r);

    let skip = naming.i32_label("sdiv_skip");
    let scratch_name = new_temp(mgr, naming, &mut insts, "sdiv", "negsc");
    let scratch = pin_named(mgr, &mut insts, &scratch_name);
    if want_rem {
        em2(mgr, &mut insts, Src::N(&nneg), Src::R(Reg::R0), |a, b| AsmInst::Beq(a, b, skip.clone()));
        insts.extend(emit_i32_neg(result_lo, result_hi, result_lo, result_hi, scratch));
        insts.push(AsmInst::Label(skip));
    } else {
        let diff = new_temp(mgr, naming, &mut insts, "sdiv", "diff");
        em3(mgr, &mut insts, Src::N(&diff), Src::N(&nneg), Src::N(&dneg), AsmInst::Xor);
        em2(mgr, &mut insts, Src::N(&diff), Src::R(Reg::R0), |a, b| AsmInst::Beq(a, b, skip.clone()));
        insts.extend(emit_i32_neg(result_lo, result_hi, result_lo, result_hi, scratch));
        insts.push(AsmInst::Label(skip));
    }
    mgr.unpin_register(scratch);
    insts
}

fn shift_amt_reg(
    mgr: &mut RegisterPressureManager,
    naming: &mut NameGenerator,
    rhs: &Value,
) -> (Reg, Vec<AsmInst>) {
    let mut insts = Vec::new();
    match rhs {
        Value::Temp(t) => {
            let name = naming.temp_name(*t);
            let reg = mgr.get_register(name);
            insts.extend(mgr.take_instructions());
            (reg, insts)
        }
        Value::Constant(c) => {
            let name = naming.imm_value(*c as i16);
            let reg = mgr.get_register(name);
            insts.extend(mgr.take_instructions());
            insts.push(AsmInst::Li(reg, (*c as i16) & 31));
            (reg, insts)
        }
        _ => panic!("shift amount must be temp or constant"),
    }
}

fn emit_i32_shl(
    result_lo: Reg,
    result_hi: Reg,
    a_lo: Reg,
    a_hi: Reg,
    amt: Reg,
    mgr: &mut RegisterPressureManager,
    naming: &mut NameGenerator,
) -> Vec<AsmInst> {
    let mut insts = Vec::new();
    let ge32 = naming.i32_label("shl_ge32");
    let ge16 = naming.i32_label("shl_ge16");
    let lt16 = naming.i32_label("shl_lt16");
    let done = naming.i32_label("shl_done");

    let c32 = new_temp(mgr, naming, &mut insts, "shl", "c32");
    em_li(mgr, &mut insts, Src::N(&c32), 32);
    let c16 = new_temp(mgr, naming, &mut insts, "shl", "c16");
    em_li(mgr, &mut insts, Src::N(&c16), 16);
    let tmp = new_temp(mgr, naming, &mut insts, "shl", "cmp");
    em3(mgr, &mut insts, Src::N(&tmp), Src::R(amt), Src::N(&c32), AsmInst::Sltu);
    em2(mgr, &mut insts, Src::N(&tmp), Src::R(Reg::R0), |a, b| AsmInst::Beq(a, b, ge32.clone()));
    em3(mgr, &mut insts, Src::N(&tmp), Src::R(amt), Src::N(&c16), AsmInst::Sltu);
    em2(mgr, &mut insts, Src::N(&tmp), Src::R(Reg::R0), |a, b| AsmInst::Beq(a, b, ge16.clone()));
    insts.push(AsmInst::Beq(Reg::R0, Reg::R0, lt16.clone()));

    insts.push(AsmInst::Label(ge32));
    insts.push(AsmInst::Li(result_lo, 0));
    insts.push(AsmInst::Li(result_hi, 0));
    insts.push(AsmInst::Beq(Reg::R0, Reg::R0, done.clone()));

    insts.push(AsmInst::Label(ge16));
    em3(mgr, &mut insts, Src::N(&tmp), Src::R(amt), Src::N(&c16), AsmInst::Sub);
    em3(mgr, &mut insts, Src::R(result_hi), Src::R(a_lo), Src::N(&tmp), AsmInst::Sll);
    insts.push(AsmInst::Li(result_lo, 0));
    insts.push(AsmInst::Beq(Reg::R0, Reg::R0, done.clone()));

    insts.push(AsmInst::Label(lt16));
    insts.push(AsmInst::Add(result_lo, a_lo, Reg::R0));
    insts.push(AsmInst::Add(result_hi, a_hi, Reg::R0));
    insts.push(AsmInst::Beq(amt, Reg::R0, done.clone()));
    insts.push(AsmInst::Sll(result_lo, a_lo, amt));
    insts.push(AsmInst::Sll(result_hi, a_hi, amt));
    em3(mgr, &mut insts, Src::N(&tmp), Src::N(&c16), Src::R(amt), AsmInst::Sub);
    let overlap = new_temp(mgr, naming, &mut insts, "shl", "ov");
    em3(mgr, &mut insts, Src::N(&overlap), Src::R(a_lo), Src::N(&tmp), AsmInst::Srl);
    em3(mgr, &mut insts, Src::R(result_hi), Src::R(result_hi), Src::N(&overlap), AsmInst::Or);
    insts.push(AsmInst::Label(done));
    insts
}

fn emit_i32_lshr(
    result_lo: Reg,
    result_hi: Reg,
    a_lo: Reg,
    a_hi: Reg,
    amt: Reg,
    mgr: &mut RegisterPressureManager,
    naming: &mut NameGenerator,
) -> Vec<AsmInst> {
    let mut insts = Vec::new();
    let ge32 = naming.i32_label("lshr_ge32");
    let ge16 = naming.i32_label("lshr_ge16");
    let lt16 = naming.i32_label("lshr_lt16");
    let done = naming.i32_label("lshr_done");

    let c32 = new_temp(mgr, naming, &mut insts, "lshr", "c32");
    em_li(mgr, &mut insts, Src::N(&c32), 32);
    let c16 = new_temp(mgr, naming, &mut insts, "lshr", "c16");
    em_li(mgr, &mut insts, Src::N(&c16), 16);
    let tmp = new_temp(mgr, naming, &mut insts, "lshr", "cmp");
    em3(mgr, &mut insts, Src::N(&tmp), Src::R(amt), Src::N(&c32), AsmInst::Sltu);
    em2(mgr, &mut insts, Src::N(&tmp), Src::R(Reg::R0), |a, b| AsmInst::Beq(a, b, ge32.clone()));
    em3(mgr, &mut insts, Src::N(&tmp), Src::R(amt), Src::N(&c16), AsmInst::Sltu);
    em2(mgr, &mut insts, Src::N(&tmp), Src::R(Reg::R0), |a, b| AsmInst::Beq(a, b, ge16.clone()));
    insts.push(AsmInst::Beq(Reg::R0, Reg::R0, lt16.clone()));

    insts.push(AsmInst::Label(ge32));
    insts.push(AsmInst::Li(result_lo, 0));
    insts.push(AsmInst::Li(result_hi, 0));
    insts.push(AsmInst::Beq(Reg::R0, Reg::R0, done.clone()));

    insts.push(AsmInst::Label(ge16));
    em3(mgr, &mut insts, Src::N(&tmp), Src::R(amt), Src::N(&c16), AsmInst::Sub);
    em3(mgr, &mut insts, Src::R(result_lo), Src::R(a_hi), Src::N(&tmp), AsmInst::Srl);
    insts.push(AsmInst::Li(result_hi, 0));
    insts.push(AsmInst::Beq(Reg::R0, Reg::R0, done.clone()));

    insts.push(AsmInst::Label(lt16));
    insts.push(AsmInst::Add(result_lo, a_lo, Reg::R0));
    insts.push(AsmInst::Add(result_hi, a_hi, Reg::R0));
    insts.push(AsmInst::Beq(amt, Reg::R0, done.clone()));
    insts.push(AsmInst::Srl(result_lo, a_lo, amt));
    insts.push(AsmInst::Srl(result_hi, a_hi, amt));
    em3(mgr, &mut insts, Src::N(&tmp), Src::N(&c16), Src::R(amt), AsmInst::Sub);
    let overlap = new_temp(mgr, naming, &mut insts, "lshr", "ov");
    em3(mgr, &mut insts, Src::N(&overlap), Src::R(a_hi), Src::N(&tmp), AsmInst::Sll);
    em3(mgr, &mut insts, Src::R(result_lo), Src::R(result_lo), Src::N(&overlap), AsmInst::Or);
    insts.push(AsmInst::Label(done));
    insts
}

fn emit_ashr16(dst: Reg, src: Reg, amt: Reg, mgr: &mut RegisterPressureManager, naming: &mut NameGenerator) -> Vec<AsmInst> {
    let mut insts = Vec::new();
    insts.push(AsmInst::Srl(dst, src, amt));
    let sign = new_temp(mgr, naming, &mut insts, "ashr", "sign");
    em3(mgr, &mut insts, Src::N(&sign), Src::R(src), Src::R(Reg::R0), AsmInst::Slt);
    let c16 = new_temp(mgr, naming, &mut insts, "ashr", "c16");
    em_li(mgr, &mut insts, Src::N(&c16), 16);
    let fill = new_temp(mgr, naming, &mut insts, "ashr", "fill");
    em3(mgr, &mut insts, Src::N(&fill), Src::N(&c16), Src::R(amt), AsmInst::Sub);
    let ones = new_temp(mgr, naming, &mut insts, "ashr", "ones");
    em_li(mgr, &mut insts, Src::N(&ones), -1);
    em3(mgr, &mut insts, Src::N(&ones), Src::N(&ones), Src::N(&fill), AsmInst::Sll);
    let skip = naming.i32_label("ashr16_pos");
    em2(mgr, &mut insts, Src::N(&sign), Src::R(Reg::R0), |a, b| AsmInst::Beq(a, b, skip.clone()));
    em3(mgr, &mut insts, Src::R(dst), Src::R(dst), Src::N(&ones), AsmInst::Or);
    insts.push(AsmInst::Label(skip));
    insts
}

fn emit_i32_ashr(
    result_lo: Reg,
    result_hi: Reg,
    a_lo: Reg,
    a_hi: Reg,
    amt: Reg,
    mgr: &mut RegisterPressureManager,
    naming: &mut NameGenerator,
) -> Vec<AsmInst> {
    let mut insts = Vec::new();
    let ge32 = naming.i32_label("ashr_ge32");
    let ge16 = naming.i32_label("ashr_ge16");
    let lt16 = naming.i32_label("ashr_lt16");
    let done = naming.i32_label("ashr_done");

    let c32 = new_temp(mgr, naming, &mut insts, "ashr", "c32");
    em_li(mgr, &mut insts, Src::N(&c32), 32);
    let c16 = new_temp(mgr, naming, &mut insts, "ashr", "c16");
    em_li(mgr, &mut insts, Src::N(&c16), 16);
    let tmp = new_temp(mgr, naming, &mut insts, "ashr", "cmp");
    em3(mgr, &mut insts, Src::N(&tmp), Src::R(amt), Src::N(&c32), AsmInst::Sltu);
    em2(mgr, &mut insts, Src::N(&tmp), Src::R(Reg::R0), |a, b| AsmInst::Beq(a, b, ge32.clone()));
    em3(mgr, &mut insts, Src::N(&tmp), Src::R(amt), Src::N(&c16), AsmInst::Sltu);
    em2(mgr, &mut insts, Src::N(&tmp), Src::R(Reg::R0), |a, b| AsmInst::Beq(a, b, ge16.clone()));
    insts.push(AsmInst::Beq(Reg::R0, Reg::R0, lt16.clone()));

    insts.push(AsmInst::Label(ge32));
    let sh15 = new_temp(mgr, naming, &mut insts, "ashr", "sh15");
    em_li(mgr, &mut insts, Src::N(&sh15), 15);
    em3(mgr, &mut insts, Src::R(result_lo), Src::R(a_hi), Src::N(&sh15), AsmInst::Srl);
    insts.push(AsmInst::Sub(result_lo, Reg::R0, result_lo));
    insts.push(AsmInst::Add(result_hi, result_lo, Reg::R0));
    insts.push(AsmInst::Beq(Reg::R0, Reg::R0, done.clone()));

    insts.push(AsmInst::Label(ge16));
    em3(mgr, &mut insts, Src::N(&tmp), Src::R(amt), Src::N(&c16), AsmInst::Sub);
    {
        let mut unpin = Vec::new();
        let t = materialize(mgr, &mut insts, Src::N(&tmp), &mut unpin);
        insts.extend(emit_ashr16(result_lo, a_hi, t, mgr, naming));
        finish_pins(mgr, unpin);
    }
    let sh15b = new_temp(mgr, naming, &mut insts, "ashr", "sh15b");
    em_li(mgr, &mut insts, Src::N(&sh15b), 15);
    em3(mgr, &mut insts, Src::R(result_hi), Src::R(a_hi), Src::N(&sh15b), AsmInst::Srl);
    insts.push(AsmInst::Sub(result_hi, Reg::R0, result_hi));
    insts.push(AsmInst::Beq(Reg::R0, Reg::R0, done.clone()));

    insts.push(AsmInst::Label(lt16));
    insts.push(AsmInst::Add(result_lo, a_lo, Reg::R0));
    insts.push(AsmInst::Add(result_hi, a_hi, Reg::R0));
    insts.push(AsmInst::Beq(amt, Reg::R0, done.clone()));
    insts.push(AsmInst::Srl(result_lo, a_lo, amt));
    insts.extend(emit_ashr16(result_hi, a_hi, amt, mgr, naming));
    em3(mgr, &mut insts, Src::N(&tmp), Src::N(&c16), Src::R(amt), AsmInst::Sub);
    let overlap = new_temp(mgr, naming, &mut insts, "ashr", "ov");
    em3(mgr, &mut insts, Src::N(&overlap), Src::R(a_hi), Src::N(&tmp), AsmInst::Sll);
    em3(mgr, &mut insts, Src::R(result_lo), Src::R(result_lo), Src::N(&overlap), AsmInst::Or);
    insts.push(AsmInst::Label(done));
    insts
}

pub fn lower_i32_binary(
    mgr: &mut RegisterPressureManager,
    naming: &mut NameGenerator,
    op: IrBinaryOp,
    lhs: &Value,
    rhs: &Value,
    result_temp: TempId,
) -> Vec<AsmInst> {
    debug!("lower_i32_binary: {op:?} t{result_temp}");
    let mut insts = Vec::new();
    let result_name = naming.temp_name(result_temp);
    let sign = matches!(
        op,
        IrBinaryOp::SDiv | IrBinaryOp::SRem | IrBinaryOp::AShr
            | IrBinaryOp::Slt | IrBinaryOp::Sle | IrBinaryOp::Sgt | IrBinaryOp::Sge
    );

    let (a_lo, a_hi, a_insts) = get_i32_pair(mgr, naming, lhs, sign);
    insts.extend(a_insts);
    mgr.pin_register(a_lo);
    mgr.pin_register(a_hi);

    let is_cmp = matches!(
        op,
        IrBinaryOp::Eq | IrBinaryOp::Ne
            | IrBinaryOp::Slt | IrBinaryOp::Sle | IrBinaryOp::Sgt | IrBinaryOp::Sge
            | IrBinaryOp::Ult | IrBinaryOp::Ule | IrBinaryOp::Ugt | IrBinaryOp::Uge
    );
    let is_shift = matches!(op, IrBinaryOp::Shl | IrBinaryOp::LShr | IrBinaryOp::AShr);

    let (b_lo, b_hi) = if is_shift {
        (Reg::R0, Reg::R0)
    } else {
        let (bl, bh, bi) = get_i32_pair(mgr, naming, rhs, sign);
        insts.extend(bi);
        mgr.pin_register(bl);
        mgr.pin_register(bh);
        (bl, bh)
    };

    let result_lo = mgr.get_register(result_name.clone());
    insts.extend(mgr.take_instructions());
    mgr.pin_register(result_lo);

    if is_cmp {
        match op {
            IrBinaryOp::Eq => {
                let xlo = new_temp(mgr, naming, &mut insts, "eq", "xlo");
                em3(mgr, &mut insts, Src::N(&xlo), Src::R(a_lo), Src::R(b_lo), AsmInst::Xor);
                let xhi = new_temp(mgr, naming, &mut insts, "eq", "xhi");
                em3(mgr, &mut insts, Src::N(&xhi), Src::R(a_hi), Src::R(b_hi), AsmInst::Xor);
                em3(mgr, &mut insts, Src::N(&xlo), Src::N(&xlo), Src::N(&xhi), AsmInst::Or);
                let one = new_temp(mgr, naming, &mut insts, "eq", "one");
                em_li(mgr, &mut insts, Src::N(&one), 1);
                em3(mgr, &mut insts, Src::R(result_lo), Src::N(&xlo), Src::N(&one), AsmInst::Sltu);
            }
            IrBinaryOp::Ne => {
                let xlo = new_temp(mgr, naming, &mut insts, "ne", "xlo");
                em3(mgr, &mut insts, Src::N(&xlo), Src::R(a_lo), Src::R(b_lo), AsmInst::Xor);
                let xhi = new_temp(mgr, naming, &mut insts, "ne", "xhi");
                em3(mgr, &mut insts, Src::N(&xhi), Src::R(a_hi), Src::R(b_hi), AsmInst::Xor);
                em3(mgr, &mut insts, Src::N(&xlo), Src::N(&xlo), Src::N(&xhi), AsmInst::Or);
                em3(mgr, &mut insts, Src::R(result_lo), Src::R(Reg::R0), Src::N(&xlo), AsmInst::Sltu);
            }
            IrBinaryOp::Ult => {
                insts.extend(emit_i32_ult(result_lo, a_lo, a_hi, b_lo, b_hi, mgr, naming));
            }
            IrBinaryOp::Ugt => {
                insts.extend(emit_i32_ult(result_lo, b_lo, b_hi, a_lo, a_hi, mgr, naming));
            }
            IrBinaryOp::Ule => {
                insts.extend(emit_i32_ult(result_lo, b_lo, b_hi, a_lo, a_hi, mgr, naming));
                insts.extend(invert01(mgr, naming, result_lo, result_lo));
            }
            IrBinaryOp::Uge => {
                insts.extend(emit_i32_ult(result_lo, a_lo, a_hi, b_lo, b_hi, mgr, naming));
                insts.extend(invert01(mgr, naming, result_lo, result_lo));
            }
            IrBinaryOp::Slt => {
                insts.extend(emit_i32_slt(result_lo, a_lo, a_hi, b_lo, b_hi, mgr, naming));
            }
            IrBinaryOp::Sgt => {
                insts.extend(emit_i32_slt(result_lo, b_lo, b_hi, a_lo, a_hi, mgr, naming));
            }
            IrBinaryOp::Sle => {
                insts.extend(emit_i32_slt(result_lo, b_lo, b_hi, a_lo, a_hi, mgr, naming));
                insts.extend(invert01(mgr, naming, result_lo, result_lo));
            }
            IrBinaryOp::Sge => {
                insts.extend(emit_i32_slt(result_lo, a_lo, a_hi, b_lo, b_hi, mgr, naming));
                insts.extend(invert01(mgr, naming, result_lo, result_lo));
            }
            _ => unreachable!(),
        }
        mgr.bind_value_to_register(result_name, result_lo);
        mgr.unpin_register(result_lo);
        mgr.unpin_register(a_lo);
        mgr.unpin_register(a_hi);
        mgr.unpin_register(b_lo);
        mgr.unpin_register(b_hi);
        return insts;
    }

    let hi_name = naming.i32_high_name(&result_name);
    let result_hi = mgr.get_register(hi_name);
    insts.extend(mgr.take_instructions());
    mgr.pin_register(result_hi);

    match op {
        IrBinaryOp::Add => {
            insts.extend(emit_i32_add(result_lo, result_hi, a_lo, a_hi, b_lo, b_hi, mgr, naming));
        }
        IrBinaryOp::Sub => {
            insts.extend(emit_i32_sub(result_lo, result_hi, a_lo, a_hi, b_lo, b_hi, mgr, naming));
        }
        IrBinaryOp::And => {
            insts.push(AsmInst::And(result_lo, a_lo, b_lo));
            insts.push(AsmInst::And(result_hi, a_hi, b_hi));
        }
        IrBinaryOp::Or => {
            insts.push(AsmInst::Or(result_lo, a_lo, b_lo));
            insts.push(AsmInst::Or(result_hi, a_hi, b_hi));
        }
        IrBinaryOp::Xor => {
            insts.push(AsmInst::Xor(result_lo, a_lo, b_lo));
            insts.push(AsmInst::Xor(result_hi, a_hi, b_hi));
        }
        IrBinaryOp::Mul => {
            insts.extend(emit_i32_mul(result_lo, result_hi, a_lo, a_hi, b_lo, b_hi, mgr, naming));
        }
        IrBinaryOp::UDiv => {
            insts.extend(emit_i32_udiv(result_lo, result_hi, a_lo, a_hi, b_lo, b_hi, false, mgr, naming));
        }
        IrBinaryOp::URem => {
            insts.extend(emit_i32_udiv(result_lo, result_hi, a_lo, a_hi, b_lo, b_hi, true, mgr, naming));
        }
        IrBinaryOp::SDiv => {
            insts.extend(emit_i32_sdiv(result_lo, result_hi, a_lo, a_hi, b_lo, b_hi, false, mgr, naming));
        }
        IrBinaryOp::SRem => {
            insts.extend(emit_i32_sdiv(result_lo, result_hi, a_lo, a_hi, b_lo, b_hi, true, mgr, naming));
        }
        IrBinaryOp::Shl | IrBinaryOp::LShr | IrBinaryOp::AShr => {
            let (amt, ai) = shift_amt_reg(mgr, naming, rhs);
            insts.extend(ai);
            mgr.pin_register(amt);
            match op {
                IrBinaryOp::Shl => {
                    insts.extend(emit_i32_shl(result_lo, result_hi, a_lo, a_hi, amt, mgr, naming));
                }
                IrBinaryOp::LShr => {
                    insts.extend(emit_i32_lshr(result_lo, result_hi, a_lo, a_hi, amt, mgr, naming));
                }
                IrBinaryOp::AShr => {
                    insts.extend(emit_i32_ashr(result_lo, result_hi, a_lo, a_hi, amt, mgr, naming));
                }
                _ => unreachable!(),
            }
            mgr.unpin_register(amt);
        }
        _ => panic!("unexpected I32 binary op {op:?}"),
    }

    bind_i32(mgr, naming, &result_name, result_lo, result_hi);
    mgr.unpin_register(result_lo);
    mgr.unpin_register(result_hi);
    mgr.unpin_register(a_lo);
    mgr.unpin_register(a_hi);
    if !is_shift {
        mgr.unpin_register(b_lo);
        mgr.unpin_register(b_hi);
    }
    insts
}

pub fn lower_i32_extend(
    mgr: &mut RegisterPressureManager,
    naming: &mut NameGenerator,
    op: IrUnaryOp,
    operand: &Value,
    result_temp: TempId,
) -> Vec<AsmInst> {
    let mut insts = Vec::new();
    let result_name = naming.temp_name(result_temp);
    let sign = matches!(op, IrUnaryOp::SExt);

    match operand {
        Value::Constant(c) => {
            let (lo, hi) = split_const(*c);
            let lo_reg = mgr.get_register(result_name.clone());
            insts.extend(mgr.take_instructions());
            insts.push(AsmInst::Li(lo_reg, lo));
            mgr.pin_register(lo_reg);
            let hi_name = naming.i32_high_name(&result_name);
            let hi_reg = mgr.get_register(hi_name);
            insts.extend(mgr.take_instructions());
            insts.push(AsmInst::Li(hi_reg, hi));
            mgr.unpin_register(lo_reg);
            bind_i32(mgr, naming, &result_name, lo_reg, hi_reg);
        }
        _ => {
            let (lo, hi, ext) = get_i32_pair(mgr, naming, operand, sign);
            insts.extend(ext);
            // Move into result registers if needed
            let result_lo = mgr.get_register(result_name.clone());
            insts.extend(mgr.take_instructions());
            if result_lo != lo {
                insts.push(AsmInst::Add(result_lo, lo, Reg::R0));
            }
            mgr.pin_register(result_lo);
            let hi_name = naming.i32_high_name(&result_name);
            let result_hi = mgr.get_register(hi_name);
            insts.extend(mgr.take_instructions());
            if result_hi != hi {
                insts.push(AsmInst::Add(result_hi, hi, Reg::R0));
            }
            mgr.unpin_register(result_lo);
            bind_i32(mgr, naming, &result_name, result_lo, result_hi);
        }
    }
    insts
}

/// OR both halves of an I32 temp (or the full constant) into a register for truthiness.
pub fn i32_to_cond_reg(
    mgr: &mut RegisterPressureManager,
    naming: &mut NameGenerator,
    value: &Value,
) -> (Reg, Vec<AsmInst>) {
    let mut insts = Vec::new();
    match value {
        Value::Constant(c) => {
            let name = naming.const_value(*c);
            let reg = mgr.get_register(name);
            insts.extend(mgr.take_instructions());
            insts.push(AsmInst::Li(reg, if *c != 0 { 1 } else { 0 }));
            (reg, insts)
        }
        Value::Temp(t) => {
            let lo_name = naming.temp_name(*t);
            let lo = mgr.get_register(lo_name.clone());
            insts.extend(mgr.take_instructions());
            if let Some(hi_name) = mgr.get_i32_high(&lo_name) {
                mgr.pin_register(lo);
                let hi = mgr.get_register(hi_name);
                insts.extend(mgr.take_instructions());
                mgr.pin_register(hi);
                let or_name = new_temp(mgr, naming, &mut insts, "cond", "or");
                em3(mgr, &mut insts, Src::N(&or_name), Src::R(lo), Src::R(hi), AsmInst::Or);
                if let Some(extra) = mgr.get_i64_words(&lo_name) {
                    for w in extra {
                        let wr = mgr.get_register(w);
                        insts.extend(mgr.take_instructions());
                        em3(mgr, &mut insts, Src::N(&or_name), Src::N(&or_name), Src::R(wr), AsmInst::Or);
                    }
                }
                mgr.unpin_register(hi);
                mgr.unpin_register(lo);
                let or_reg = mgr.get_register(or_name);
                insts.extend(mgr.take_instructions());
                (or_reg, insts)
            } else if let Some(extra) = mgr.get_i64_words(&lo_name) {
                mgr.pin_register(lo);
                let or_name = new_temp(mgr, naming, &mut insts, "cond", "or");
                em3(mgr, &mut insts, Src::N(&or_name), Src::R(lo), Src::R(Reg::R0), AsmInst::Add);
                for w in extra {
                    let wr = mgr.get_register(w);
                    insts.extend(mgr.take_instructions());
                    em3(mgr, &mut insts, Src::N(&or_name), Src::N(&or_name), Src::R(wr), AsmInst::Or);
                }
                mgr.unpin_register(lo);
                let or_reg = mgr.get_register(or_name);
                insts.extend(mgr.take_instructions());
                (or_reg, insts)
            } else {
                (lo, insts)
            }
        }
        _ => {
            let lo = crate::instr::helpers::get_value_register(mgr, naming, value);
            insts.extend(mgr.take_instructions());
            (lo, insts)
        }
    }
}
