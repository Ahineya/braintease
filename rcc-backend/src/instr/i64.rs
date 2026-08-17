//! 64-bit (I64 / `long long`) lowering: four 16-bit words, little-endian.
//!
//! Low word in the named temp, then `{name}__w1`, `{name}__w2`, `{name}__w3`.

use rcc_frontend::ir::{IrBinaryOp, IrUnaryOp, Value};
use rcc_common::TempId;
use crate::regmgmt::RegisterPressureManager;
use crate::naming::NameGenerator;
use rcc_codegen::{AsmInst, Reg};
use log::debug;

pub fn split_const64(c: i64) -> [i16; 4] {
    let u = c as u64;
    [
        (u & 0xFFFF) as i16,
        ((u >> 16) & 0xFFFF) as i16,
        ((u >> 32) & 0xFFFF) as i16,
        ((u >> 48) & 0xFFFF) as i16,
    ]
}

pub fn value_is_i64(
    mgr: &RegisterPressureManager,
    naming: &NameGenerator,
    value: &Value,
) -> bool {
    match value {
        Value::Temp(t) => mgr.get_i64_words(&naming.temp_name(*t)).is_some(),
        Value::Constant(c) => *c < i32::MIN as i64 || *c > u32::MAX as i64,
        _ => false,
    }
}

pub fn bind_i64(
    mgr: &mut RegisterPressureManager,
    naming: &NameGenerator,
    lo_name: &str,
    regs: [Reg; 4],
) {
    let words = [
        naming.i64_word_name(lo_name, 1),
        naming.i64_word_name(lo_name, 2),
        naming.i64_word_name(lo_name, 3),
    ];
    mgr.bind_value_to_register(lo_name.to_string(), regs[0]);
    for i in 0..3 {
        mgr.bind_value_to_register(words[i].clone(), regs[i + 1]);
    }
    mgr.set_i64_words(lo_name.to_string(), words);
}

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
    let mut unpin = Vec::new();
    let r = materialize(mgr, insts, dst, &mut unpin);
    insts.push(AsmInst::Li(r, imm));
    finish_pins(mgr, unpin);
}

fn extend_i16(
    mgr: &mut RegisterPressureManager,
    naming: &mut NameGenerator,
    src: Reg,
    sign_extend: bool,
) -> (Reg, Vec<AsmInst>) {
    let mut insts = Vec::new();
    let hi_name = naming.temp_with_context("i64", if sign_extend { "sext16" } else { "zext16" });
    let hi = mgr.get_register(hi_name);
    insts.extend(mgr.take_instructions());
    if sign_extend {
        let sh = mgr.get_register(naming.temp_with_context("i64", "sh15"));
        insts.extend(mgr.take_instructions());
        insts.push(AsmInst::Li(sh, 15));
        insts.push(AsmInst::Srl(hi, src, sh));
        insts.push(AsmInst::Sub(hi, Reg::R0, hi));
        mgr.free_register(sh);
    } else {
        insts.push(AsmInst::Li(hi, 0));
    }
    (hi, insts)
}

/// Materialize four words for an I64 operand.
pub fn get_i64_quad(
    mgr: &mut RegisterPressureManager,
    naming: &mut NameGenerator,
    value: &Value,
    sign_extend: bool,
) -> ([Reg; 4], Vec<AsmInst>) {
    let mut insts = Vec::new();
    match value {
        Value::Temp(t) => {
            let lo_name = naming.temp_name(*t);
            let w0 = mgr.get_register(lo_name.clone());
            insts.extend(mgr.take_instructions());
            if let Some(words) = mgr.get_i64_words(&lo_name) {
                let w1 = mgr.get_register(words[0].clone());
                insts.extend(mgr.take_instructions());
                let w2 = mgr.get_register(words[1].clone());
                insts.extend(mgr.take_instructions());
                let w3 = mgr.get_register(words[2].clone());
                insts.extend(mgr.take_instructions());
                ([w0, w1, w2, w3], insts)
            } else if let Some(hi_name) = mgr.get_i32_high(&lo_name) {
                let w1 = mgr.get_register(hi_name);
                insts.extend(mgr.take_instructions());
                mgr.pin_register(w0);
                mgr.pin_register(w1);
                let (fill, ext) = extend_i16(mgr, naming, w1, sign_extend);
                insts.extend(ext);
                let w2 = fill;
                mgr.pin_register(w2);
                let w3_name = naming.temp_with_context("i64", "from32_w3");
                let w3 = mgr.get_register(w3_name);
                insts.extend(mgr.take_instructions());
                insts.push(AsmInst::Add(w3, w2, Reg::R0));
                mgr.unpin_register(w2);
                mgr.unpin_register(w1);
                mgr.unpin_register(w0);
                ([w0, w1, w2, w3], insts)
            } else {
                mgr.pin_register(w0);
                let (fill, ext) = extend_i16(mgr, naming, w0, sign_extend);
                insts.extend(ext);
                mgr.pin_register(fill);
                let w2_name = naming.temp_with_context("i64", "from16_w2");
                let w2 = mgr.get_register(w2_name);
                insts.extend(mgr.take_instructions());
                insts.push(AsmInst::Add(w2, fill, Reg::R0));
                let w3_name = naming.temp_with_context("i64", "from16_w3");
                let w3 = mgr.get_register(w3_name);
                insts.extend(mgr.take_instructions());
                insts.push(AsmInst::Add(w3, fill, Reg::R0));
                mgr.unpin_register(fill);
                mgr.unpin_register(w0);
                ([w0, fill, w2, w3], insts)
            }
        }
        Value::Constant(c) => {
            let parts = split_const64(*c);
            let mut regs = [Reg::R0; 4];
            for i in 0..4 {
                let name = naming.temp_with_context("i64", &format!("c{i}"));
                let r = mgr.get_register(name);
                insts.extend(mgr.take_instructions());
                insts.push(AsmInst::Li(r, parts[i]));
                regs[i] = r;
            }
            (regs, insts)
        }
        _ => panic!("I64 operand must be a temp or constant, got {value:?}"),
    }
}

/// Add/sub carry scratches. Not allocatable, so they stay valid while all
/// 12 T/S registers are pinned for a 4+4+4-word operation.
const I64_CY: Reg = Reg::X2;
const I64_TMP: Reg = Reg::X3;

/// Park a 4-word I64 in non-allocatable ABI regs so T/S pins stay under 12.
/// Same split as I64 udiv: quotient-side in Rv/X, divisor-side in A0–A3.
const I64_PARK_A: [Reg; 4] = [Reg::Rv0, Reg::Rv1, Reg::X0, Reg::X1];
const I64_PARK_B: [Reg; 4] = [Reg::A0, Reg::A1, Reg::A2, Reg::A3];

fn park_quad(dst: [Reg; 4], src: [Reg; 4]) -> Vec<AsmInst> {
    (0..4)
        .filter_map(|i| {
            if dst[i] != src[i] {
                Some(AsmInst::Add(dst[i], src[i], Reg::R0))
            } else {
                None
            }
        })
        .collect()
}

fn unpin_quad(mgr: &mut RegisterPressureManager, regs: [Reg; 4]) {
    for r in regs {
        mgr.unpin_register(r);
    }
}

fn emit_i64_add(
    result: [Reg; 4],
    a: [Reg; 4],
    b: [Reg; 4],
) -> Vec<AsmInst> {
    let mut insts = vec![AsmInst::Li(Reg::Sc, 0)];
    for i in 0..4 {
        insts.push(AsmInst::Add(I64_TMP, a[i], b[i]));
        insts.push(AsmInst::Sltu(I64_CY, I64_TMP, a[i]));
        insts.push(AsmInst::Add(result[i], I64_TMP, Reg::Sc));
        insts.push(AsmInst::Sltu(Reg::Sc, result[i], I64_TMP));
        insts.push(AsmInst::Or(Reg::Sc, Reg::Sc, I64_CY));
    }
    insts
}

fn emit_i64_sub(
    result: [Reg; 4],
    a: [Reg; 4],
    b: [Reg; 4],
) -> Vec<AsmInst> {
    let mut insts = vec![AsmInst::Li(Reg::Sc, 0)];
    for i in 0..4 {
        insts.push(AsmInst::Sltu(I64_CY, a[i], b[i]));
        insts.push(AsmInst::Sub(I64_TMP, a[i], b[i]));
        insts.push(AsmInst::Sub(result[i], I64_TMP, Reg::Sc));
        insts.push(AsmInst::Sltu(I64_TMP, I64_TMP, Reg::Sc));
        insts.push(AsmInst::Or(Reg::Sc, I64_CY, I64_TMP));
    }
    insts
}

fn emit_i64_bitwise(
    op: fn(Reg, Reg, Reg) -> AsmInst,
    result: [Reg; 4],
    a: [Reg; 4],
    b: [Reg; 4],
) -> Vec<AsmInst> {
    (0..4).map(|i| op(result[i], a[i], b[i])).collect()
}

fn emit_i64_mul(
    result_names: &[String; 4],
    a: [Reg; 4],
    b: [Reg; 4],
    mgr: &mut RegisterPressureManager,
    naming: &mut NameGenerator,
) -> Vec<AsmInst> {
    let mut insts = Vec::new();
    let mut a_n = [String::new(), String::new(), String::new(), String::new()];
    let mut b_n = [String::new(), String::new(), String::new(), String::new()];
    for i in 0..4 {
        a_n[i] = new_temp(mgr, naming, &mut insts, "mul64", &format!("aw{i}"));
        em3(mgr, &mut insts, Src::N(&a_n[i]), Src::R(a[i]), Src::R(Reg::R0), AsmInst::Add);
        b_n[i] = new_temp(mgr, naming, &mut insts, "mul64", &format!("bw{i}"));
        em3(mgr, &mut insts, Src::N(&b_n[i]), Src::R(b[i]), Src::R(Reg::R0), AsmInst::Add);
    }
    for r in a {
        mgr.unpin_register(r);
    }
    for r in b {
        mgr.unpin_register(r);
    }
    let acc: [String; 4] = [
        new_temp(mgr, naming, &mut insts, "mul64", "a0"),
        new_temp(mgr, naming, &mut insts, "mul64", "a1"),
        new_temp(mgr, naming, &mut insts, "mul64", "a2"),
        new_temp(mgr, naming, &mut insts, "mul64", "a3"),
    ];
    for name in &acc {
        em_li(mgr, &mut insts, Src::N(name), 0);
    }

    for i in 0..4 {
        for j in 0..4 {
            let dest = i + j;
            if dest >= 4 {
                continue;
            }
            let ai = {
                let r = mgr.get_register(a_n[i].clone());
                insts.extend(mgr.take_instructions());
                mgr.pin_register(r);
                r
            };
            let bj = {
                let r = mgr.get_register(b_n[j].clone());
                insts.extend(mgr.take_instructions());
                mgr.pin_register(r);
                r
            };
            let plo = new_temp(mgr, naming, &mut insts, "mul64", &format!("p{i}{j}lo"));
            let phi = new_temp(mgr, naming, &mut insts, "mul64", &format!("p{i}{j}hi"));
            let plo_r = {
                let r = mgr.get_register(plo.clone());
                insts.extend(mgr.take_instructions());
                r
            };
            let phi_r = {
                let r = mgr.get_register(phi.clone());
                insts.extend(mgr.take_instructions());
                r
            };
            mgr.pin_register(plo_r);
            mgr.pin_register(phi_r);
            insts.extend(crate::instr::wide::emit_umul16x16(
                plo_r, phi_r, ai, bj, mgr, naming,
            ));
            mgr.unpin_register(plo_r);
            mgr.unpin_register(phi_r);
            mgr.unpin_register(ai);
            mgr.unpin_register(bj);

            add_into_acc(mgr, naming, &mut insts, &acc, dest, &plo);
            if dest + 1 < 4 {
                add_into_acc(mgr, naming, &mut insts, &acc, dest + 1, &phi);
            }
        }
    }

    for i in 0..4 {
        em3(
            mgr,
            &mut insts,
            Src::N(&result_names[i]),
            Src::N(&acc[i]),
            Src::R(Reg::R0),
            AsmInst::Add,
        );
    }
    insts
}

fn add_into_acc(
    mgr: &mut RegisterPressureManager,
    naming: &mut NameGenerator,
    insts: &mut Vec<AsmInst>,
    acc: &[String; 4],
    idx: usize,
    src: &str,
) {
    let orig = new_temp(mgr, naming, insts, "mul64", "orig");
    em3(mgr, insts, Src::N(&orig), Src::N(&acc[idx]), Src::R(Reg::R0), AsmInst::Add);
    em3(mgr, insts, Src::N(&acc[idx]), Src::N(&acc[idx]), Src::N(src), AsmInst::Add);
    let carry = new_temp(mgr, naming, insts, "mul64", "cy");
    em3(mgr, insts, Src::N(&carry), Src::N(&acc[idx]), Src::N(&orig), AsmInst::Sltu);
    let mut k = idx + 1;
    let mut cy = carry;
    while k < 4 {
        em3(mgr, insts, Src::N(&acc[k]), Src::N(&acc[k]), Src::N(&cy), AsmInst::Add);
        let next = new_temp(mgr, naming, insts, "mul64", &format!("cy{k}"));
        em3(mgr, insts, Src::N(&next), Src::N(&acc[k]), Src::N(&cy), AsmInst::Sltu);
        cy = next;
        k += 1;
    }
}

fn emit_i64_ult(
    result: Reg,
    a: [Reg; 4],
    b: [Reg; 4],
    naming: &mut NameGenerator,
) -> Vec<AsmInst> {
    // Compare from high word down.
    let mut insts = Vec::new();
    let w3ne = naming.i64_label("ult_w3ne");
    let w2ne = naming.i64_label("ult_w2ne");
    let w1ne = naming.i64_label("ult_w1ne");
    let done = naming.i64_label("ult_done");
    insts.push(AsmInst::Xor(result, a[3], b[3]));
    insts.push(AsmInst::Bne(result, Reg::R0, w3ne.clone()));
    insts.push(AsmInst::Xor(result, a[2], b[2]));
    insts.push(AsmInst::Bne(result, Reg::R0, w2ne.clone()));
    insts.push(AsmInst::Xor(result, a[1], b[1]));
    insts.push(AsmInst::Bne(result, Reg::R0, w1ne.clone()));
    insts.push(AsmInst::Sltu(result, a[0], b[0]));
    insts.push(AsmInst::Beq(Reg::R0, Reg::R0, done.clone()));
    insts.push(AsmInst::Label(w3ne));
    insts.push(AsmInst::Sltu(result, a[3], b[3]));
    insts.push(AsmInst::Beq(Reg::R0, Reg::R0, done.clone()));
    insts.push(AsmInst::Label(w2ne));
    insts.push(AsmInst::Sltu(result, a[2], b[2]));
    insts.push(AsmInst::Beq(Reg::R0, Reg::R0, done.clone()));
    insts.push(AsmInst::Label(w1ne));
    insts.push(AsmInst::Sltu(result, a[1], b[1]));
    insts.push(AsmInst::Label(done));
    insts
}

fn emit_i64_slt(
    result: Reg,
    a: [Reg; 4],
    b: [Reg; 4],
    naming: &mut NameGenerator,
) -> Vec<AsmInst> {
    let mut insts = Vec::new();
    let w3ne = naming.i64_label("slt_w3ne");
    let w2ne = naming.i64_label("slt_w2ne");
    let w1ne = naming.i64_label("slt_w1ne");
    let done = naming.i64_label("slt_done");
    insts.push(AsmInst::Xor(result, a[3], b[3]));
    insts.push(AsmInst::Bne(result, Reg::R0, w3ne.clone()));
    insts.push(AsmInst::Xor(result, a[2], b[2]));
    insts.push(AsmInst::Bne(result, Reg::R0, w2ne.clone()));
    insts.push(AsmInst::Xor(result, a[1], b[1]));
    insts.push(AsmInst::Bne(result, Reg::R0, w1ne.clone()));
    insts.push(AsmInst::Sltu(result, a[0], b[0]));
    insts.push(AsmInst::Beq(Reg::R0, Reg::R0, done.clone()));
    insts.push(AsmInst::Label(w3ne));
    insts.push(AsmInst::Slt(result, a[3], b[3]));
    insts.push(AsmInst::Beq(Reg::R0, Reg::R0, done.clone()));
    insts.push(AsmInst::Label(w2ne));
    insts.push(AsmInst::Sltu(result, a[2], b[2]));
    insts.push(AsmInst::Beq(Reg::R0, Reg::R0, done.clone()));
    insts.push(AsmInst::Label(w1ne));
    insts.push(AsmInst::Sltu(result, a[1], b[1]));
    insts.push(AsmInst::Label(done));
    insts
}

fn invert01(
    mgr: &mut RegisterPressureManager,
    naming: &mut NameGenerator,
    dst: Reg,
    src: Reg,
) -> Vec<AsmInst> {
    let mut insts = Vec::new();
    let one = new_temp(mgr, naming, &mut insts, "cmp", "inv1");
    em_li(mgr, &mut insts, Src::N(&one), 1);
    em3(mgr, &mut insts, Src::R(dst), Src::N(&one), Src::R(src), AsmInst::Sub);
    insts
}

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

fn shl1_quad(words: [Reg; 4], scratch: Reg) -> Vec<AsmInst> {
    // words <<= 1. scratch is msb of each word before shift.
    vec![
        AsmInst::Slt(scratch, words[2], Reg::R0),
        AsmInst::Add(words[3], words[3], words[3]),
        AsmInst::Or(words[3], words[3], scratch),
        AsmInst::Slt(scratch, words[1], Reg::R0),
        AsmInst::Add(words[2], words[2], words[2]),
        AsmInst::Or(words[2], words[2], scratch),
        AsmInst::Slt(scratch, words[0], Reg::R0),
        AsmInst::Add(words[1], words[1], words[1]),
        AsmInst::Or(words[1], words[1], scratch),
        AsmInst::Add(words[0], words[0], words[0]),
    ]
}

/// Copy `src` into `result` shifted by `word` 16-bit limbs. `shl` selects direction.
fn emit_i64_word_shuffle(
    result: [Reg; 4],
    src: [Reg; 4],
    word: usize,
    shl: bool,
) -> Vec<AsmInst> {
    let mut insts = Vec::new();
    for i in 0..4 {
        let s = if shl {
            i as i32 - word as i32
        } else {
            i as i32 + word as i32
        };
        if s < 0 || s >= 4 {
            insts.push(AsmInst::Li(result[i], 0));
        } else {
            insts.push(AsmInst::Add(result[i], src[s as usize], Reg::R0));
        }
    }
    insts
}

/// In-place 1–15 bit shl. `overlap` must be distinct from `result` (X0 after park copy).
fn emit_i64_bit_shl_inplace(
    result: [Reg; 4],
    bit: Reg,
    sh_left: Reg,
    overlap: Reg,
) -> Vec<AsmInst> {
    let mut insts = Vec::new();
    for i in (1..4).rev() {
        insts.push(AsmInst::Srl(overlap, result[i - 1], sh_left));
        insts.push(AsmInst::Sll(result[i], result[i], bit));
        insts.push(AsmInst::Or(result[i], result[i], overlap));
    }
    insts.push(AsmInst::Sll(result[0], result[0], bit));
    insts
}

/// In-place 1–15 bit logical shr.
fn emit_i64_bit_shr_inplace(
    result: [Reg; 4],
    bit: Reg,
    sh_left: Reg,
    overlap: Reg,
) -> Vec<AsmInst> {
    let mut insts = Vec::new();
    for i in 0..3 {
        insts.push(AsmInst::Sll(overlap, result[i + 1], sh_left));
        insts.push(AsmInst::Srl(result[i], result[i], bit));
        insts.push(AsmInst::Or(result[i], result[i], overlap));
    }
    insts.push(AsmInst::Srl(result[3], result[3], bit));
    insts
}

fn emit_i64_ashr_fill(
    result: [Reg; 4],
    word: usize,
    bit: Reg,
    sign: Reg,
    naming: &mut NameGenerator,
) -> Vec<AsmInst> {
    let skip = naming.i64_label("ashr_pos");
    let mut insts = vec![AsmInst::Beq(sign, Reg::R0, skip.clone())];
    insts.push(AsmInst::Li(Reg::Rv0, -1));
    for i in (4 - word)..4 {
        insts.push(AsmInst::Add(result[i], Reg::Rv0, Reg::R0));
    }
    if word < 4 {
        let nopart = naming.i64_label("ashr_nopart");
        insts.push(AsmInst::Beq(bit, Reg::R0, nopart.clone()));
        insts.push(AsmInst::Li(Reg::Sc, 16));
        insts.push(AsmInst::Sub(Reg::Sc, Reg::Sc, bit));
        insts.push(AsmInst::Sll(I64_CY, Reg::Rv0, Reg::Sc));
        insts.push(AsmInst::Or(result[3 - word], result[3 - word], I64_CY));
        insts.push(AsmInst::Label(nopart));
    }
    insts.push(AsmInst::Label(skip));
    insts
}

/// Word-shuffle `src` into `result`, then a 0–15 bit shift. `bit` is 0–15.
fn emit_i64_shift_parts(
    result: [Reg; 4],
    src: [Reg; 4],
    word: usize,
    bit: Reg,
    shl: bool,
    arithmetic: bool,
    sign: Option<Reg>,
    naming: &mut NameGenerator,
) -> Vec<AsmInst> {
    let overlap = Reg::X0;
    let mut insts = emit_i64_word_shuffle(result, src, word, shl);
    let skip_bits = naming.i64_label("sh_nobit");
    insts.push(AsmInst::Beq(bit, Reg::R0, skip_bits.clone()));
    insts.push(AsmInst::Li(Reg::Sc, 16));
    insts.push(AsmInst::Sub(Reg::Sc, Reg::Sc, bit));
    if shl {
        insts.extend(emit_i64_bit_shl_inplace(result, bit, Reg::Sc, overlap));
    } else {
        insts.extend(emit_i64_bit_shr_inplace(result, bit, Reg::Sc, overlap));
    }
    insts.push(AsmInst::Label(skip_bits));
    if arithmetic {
        if let Some(sign) = sign {
            insts.extend(emit_i64_ashr_fill(result, word, bit, sign, naming));
        }
    }
    insts
}

fn emit_i64_shift_const(
    result: [Reg; 4],
    src: [Reg; 4],
    n: u32,
    shl: bool,
    arithmetic: bool,
    naming: &mut NameGenerator,
) -> Vec<AsmInst> {
    let n = n & 63;
    let word = (n / 16) as usize;
    let bit = n % 16;
    let mut insts = Vec::new();
    let sign = if arithmetic {
        insts.push(AsmInst::Slt(I64_PARK_B[3], src[3], Reg::R0));
        Some(I64_PARK_B[3])
    } else {
        None
    };
    if bit == 0 {
        insts.extend(emit_i64_word_shuffle(result, src, word, shl));
        if arithmetic {
            if let Some(sign) = sign {
                insts.push(AsmInst::Li(I64_CY, 0));
                insts.extend(emit_i64_ashr_fill(result, word, I64_CY, sign, naming));
            }
        }
        return insts;
    }
    insts.push(AsmInst::Li(I64_TMP, bit as i16));
    insts.extend(emit_i64_shift_parts(
        result, src, word, I64_TMP, shl, arithmetic, sign, naming,
    ));
    insts
}

/// Variable I64 shift. `amt` is already in X2 and masked to 0–63. `src` is parked
/// in Rv/X. Uses A0–A2 for 16/32/48; no extra T/S pins, no bit-by-bit loop.
fn emit_i64_shift_var(
    result: [Reg; 4],
    src: [Reg; 4],
    amt: Reg,
    shl: bool,
    arithmetic: bool,
    naming: &mut NameGenerator,
) -> Vec<AsmInst> {
    let mut insts = Vec::new();
    let sign = if arithmetic {
        insts.push(AsmInst::Slt(I64_PARK_B[3], src[3], Reg::R0));
        Some(I64_PARK_B[3])
    } else {
        None
    };
    insts.push(AsmInst::Li(I64_PARK_B[0], 16));
    insts.push(AsmInst::Li(I64_PARK_B[1], 32));
    insts.push(AsmInst::Li(I64_PARK_B[2], 48));

    let ge32 = naming.i64_label("sh_ge32");
    let ge48 = naming.i64_label("sh_ge48");
    let ge16 = naming.i64_label("sh_ge16");
    let lt16 = naming.i64_label("sh_lt16");
    let done = naming.i64_label("sh_done");

    insts.push(AsmInst::Sltu(I64_TMP, amt, I64_PARK_B[1]));
    insts.push(AsmInst::Beq(I64_TMP, Reg::R0, ge32.clone()));
    insts.push(AsmInst::Sltu(I64_TMP, amt, I64_PARK_B[0]));
    insts.push(AsmInst::Beq(I64_TMP, Reg::R0, ge16.clone()));
    insts.push(AsmInst::Beq(Reg::R0, Reg::R0, lt16.clone()));

    insts.push(AsmInst::Label(ge32));
    insts.push(AsmInst::Sltu(I64_TMP, amt, I64_PARK_B[2]));
    insts.push(AsmInst::Beq(I64_TMP, Reg::R0, ge48.clone()));
    insts.push(AsmInst::Sub(I64_TMP, amt, I64_PARK_B[1]));
    insts.extend(emit_i64_shift_parts(
        result, src, 2, I64_TMP, shl, arithmetic, sign, naming,
    ));
    insts.push(AsmInst::Beq(Reg::R0, Reg::R0, done.clone()));

    insts.push(AsmInst::Label(ge48));
    insts.push(AsmInst::Sub(I64_TMP, amt, I64_PARK_B[2]));
    insts.extend(emit_i64_shift_parts(
        result, src, 3, I64_TMP, shl, arithmetic, sign, naming,
    ));
    insts.push(AsmInst::Beq(Reg::R0, Reg::R0, done.clone()));

    insts.push(AsmInst::Label(ge16));
    insts.push(AsmInst::Sub(I64_TMP, amt, I64_PARK_B[0]));
    insts.extend(emit_i64_shift_parts(
        result, src, 1, I64_TMP, shl, arithmetic, sign, naming,
    ));
    insts.push(AsmInst::Beq(Reg::R0, Reg::R0, done.clone()));

    insts.push(AsmInst::Label(lt16));
    insts.extend(emit_i64_shift_parts(
        result, src, 0, amt, shl, arithmetic, sign, naming,
    ));
    insts.push(AsmInst::Label(done));
    insts
}

fn emit_i64_neg(
    dst: [Reg; 4],
    src: [Reg; 4],
) -> Vec<AsmInst> {
    emit_i64_sub(dst, [Reg::R0, Reg::R0, Reg::R0, Reg::R0], src)
}

fn emit_i64_udiv(
    n: [Reg; 4],
    d: [Reg; 4],
    want_rem: bool,
    mgr: &mut RegisterPressureManager,
    naming: &mut NameGenerator,
    out_names: &[String; 4],
) -> Vec<AsmInst> {
    let mut insts = Vec::new();
    insts.push(AsmInst::Comment("i64 unsigned div/rem (restoring)".to_string()));

    // Pin operands before any allocation so get_register cannot steal them.
    for r in n {
        mgr.pin_register(r);
    }
    for r in d {
        mgr.pin_register(r);
    }

    // Park divisor in A0–A3 first so copying the numerator cannot sit at 12 T/S pins.
    let dv = [Reg::A0, Reg::A1, Reg::A2, Reg::A3];
    for i in 0..4 {
        insts.push(AsmInst::Add(dv[i], d[i], Reg::R0));
    }
    for r in d {
        if !n.contains(&r) {
            mgr.unpin_register(r);
        }
    }

    let mut wn = [Reg::R0; 4];
    for i in 0..4 {
        wn[i] = alloc_pin(mgr, naming, &mut insts, "div64", &format!("n{i}"));
        insts.push(AsmInst::Add(wn[i], n[i], Reg::R0));
    }
    for r in n {
        mgr.unpin_register(r);
    }

    let mut rem = [Reg::R0; 4];
    for i in 0..4 {
        rem[i] = alloc_pin(mgr, naming, &mut insts, "div64", &format!("r{i}"));
        insts.push(AsmInst::Li(rem[i], 0));
    }
    let bits = alloc_pin(mgr, naming, &mut insts, "div64", "bits");
    let scratch = alloc_pin(mgr, naming, &mut insts, "div64", "scr");

    let q = [Reg::Rv0, Reg::Rv1, Reg::X0, Reg::X1];
    for i in 0..4 {
        insts.push(AsmInst::Li(q[i], 0));
    }
    insts.push(AsmInst::Li(bits, 64));

    let loop_l = naming.i64_label("udiv_loop");
    let ge_l = naming.i64_label("udiv_ge");
    let next_l = naming.i64_label("udiv_next");
    let done_l = naming.i64_label("udiv_done");

    insts.push(AsmInst::Label(loop_l.clone()));
    insts.extend(shl1_quad(rem, scratch));
    insts.push(AsmInst::Slt(scratch, wn[3], Reg::R0));
    insts.push(AsmInst::Or(rem[0], rem[0], scratch));
    insts.extend(shl1_quad(wn, scratch));
    insts.extend(shl1_quad(q, scratch));
    insts.extend(emit_i64_ult(scratch, rem, dv, naming));
    insts.push(AsmInst::Bne(scratch, Reg::R0, next_l.clone()));
    insts.push(AsmInst::Beq(Reg::R0, Reg::R0, ge_l.clone()));

    insts.push(AsmInst::Label(ge_l));
    insts.extend(emit_i64_sub(rem, rem, dv));
    insts.push(AsmInst::Li(scratch, 1));
    insts.push(AsmInst::Or(q[0], q[0], scratch));

    insts.push(AsmInst::Label(next_l));
    insts.push(AsmInst::AddI(bits, bits, -1));
    insts.push(AsmInst::Beq(bits, Reg::R0, done_l.clone()));
    insts.push(AsmInst::Beq(Reg::R0, Reg::R0, loop_l));

    insts.push(AsmInst::Label(done_l));
    if want_rem {
        for i in 0..4 {
            insts.push(AsmInst::Add(q[i], rem[i], Reg::R0));
        }
    }
    for r in rem {
        mgr.unpin_register(r);
    }
    for r in wn {
        mgr.unpin_register(r);
    }
    mgr.unpin_register(bits);
    mgr.unpin_register(scratch);
    for i in 0..4 {
        let r = mgr.get_register(out_names[i].clone());
        insts.extend(mgr.take_instructions());
        if r != q[i] {
            insts.push(AsmInst::Add(r, q[i], Reg::R0));
        }
        mgr.bind_value_to_register(out_names[i].clone(), r);
    }
    insts
}

fn emit_i64_sdiv(
    n: [Reg; 4],
    d: [Reg; 4],
    want_rem: bool,
    mgr: &mut RegisterPressureManager,
    naming: &mut NameGenerator,
    out_names: &[String; 4],
) -> Vec<AsmInst> {
    let mut insts = Vec::new();
    for r in n {
        mgr.pin_register(r);
    }
    for r in d {
        mgr.pin_register(r);
    }

    let nneg = new_temp(mgr, naming, &mut insts, "sdiv64", "nneg");
    em3(mgr, &mut insts, Src::N(&nneg), Src::R(n[3]), Src::R(Reg::R0), AsmInst::Slt);
    let dneg = new_temp(mgr, naming, &mut insts, "sdiv64", "dneg");
    em3(mgr, &mut insts, Src::N(&dneg), Src::R(d[3]), Src::R(Reg::R0), AsmInst::Slt);

    let mut an = [Reg::R0; 4];
    for i in 0..4 {
        an[i] = alloc_pin(mgr, naming, &mut insts, "sdiv64", &format!("an{i}"));
        insts.push(AsmInst::Add(an[i], n[i], Reg::R0));
    }
    for r in n {
        if !d.contains(&r) {
            mgr.unpin_register(r);
        }
    }

    let nneg_r = alloc_pin(mgr, naming, &mut insts, "sdiv64", "nneg_p");
    em3(mgr, &mut insts, Src::R(nneg_r), Src::N(&nneg), Src::R(Reg::R0), AsmInst::Add);
    let npos = naming.i64_label("sdiv_npos");
    insts.push(AsmInst::Beq(nneg_r, Reg::R0, npos.clone()));
    insts.extend(emit_i64_neg(an, an));
    insts.push(AsmInst::Label(npos));
    mgr.unpin_register(nneg_r);

    let mut ad = [Reg::R0; 4];
    for i in 0..4 {
        ad[i] = alloc_pin(mgr, naming, &mut insts, "sdiv64", &format!("ad{i}"));
        insts.push(AsmInst::Add(ad[i], d[i], Reg::R0));
    }
    for r in d {
        mgr.unpin_register(r);
    }

    let dneg_r = alloc_pin(mgr, naming, &mut insts, "sdiv64", "dneg_p");
    em3(mgr, &mut insts, Src::R(dneg_r), Src::N(&dneg), Src::R(Reg::R0), AsmInst::Add);
    let dpos = naming.i64_label("sdiv_dpos");
    insts.push(AsmInst::Beq(dneg_r, Reg::R0, dpos.clone()));
    insts.extend(emit_i64_neg(ad, ad));
    insts.push(AsmInst::Label(dpos));
    mgr.unpin_register(dneg_r);

    unpin_quad(mgr, an);
    unpin_quad(mgr, ad);
    insts.extend(emit_i64_udiv(an, ad, want_rem, mgr, naming, out_names));

    let nneg_r = alloc_pin(mgr, naming, &mut insts, "sdiv64", "nneg_r");
    em3(mgr, &mut insts, Src::R(nneg_r), Src::N(&nneg), Src::R(Reg::R0), AsmInst::Add);
    let dneg_r = alloc_pin(mgr, naming, &mut insts, "sdiv64", "dneg_r");
    em3(mgr, &mut insts, Src::R(dneg_r), Src::N(&dneg), Src::R(Reg::R0), AsmInst::Add);

    let mut out = [Reg::R0; 4];
    for i in 0..4 {
        out[i] = mgr.get_register(out_names[i].clone());
        insts.extend(mgr.take_instructions());
        mgr.pin_register(out[i]);
    }

    let skip = naming.i64_label("sdiv_skip");
    if want_rem {
        insts.push(AsmInst::Beq(nneg_r, Reg::R0, skip.clone()));
        insts.extend(emit_i64_neg(out, out));
        insts.push(AsmInst::Label(skip));
    } else {
        insts.push(AsmInst::Xor(dneg_r, nneg_r, dneg_r));
        insts.push(AsmInst::Beq(dneg_r, Reg::R0, skip.clone()));
        insts.extend(emit_i64_neg(out, out));
        insts.push(AsmInst::Label(skip));
    }
    mgr.unpin_register(nneg_r);
    mgr.unpin_register(dneg_r);
    for r in out {
        mgr.unpin_register(r);
    }
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
            let name = naming.imm_value((*c as i16) & 63);
            let reg = mgr.get_register(name);
            insts.extend(mgr.take_instructions());
            insts.push(AsmInst::Li(reg, (*c as i16) & 63));
            (reg, insts)
        }
        _ => panic!("shift amount must be temp or constant"),
    }
}

pub fn lower_i64_binary(
    mgr: &mut RegisterPressureManager,
    naming: &mut NameGenerator,
    op: IrBinaryOp,
    lhs: &Value,
    rhs: &Value,
    result_temp: TempId,
) -> Vec<AsmInst> {
    debug!("lower_i64_binary: {op:?} t{result_temp}");
    let mut insts = Vec::new();
    let result_name = naming.temp_name(result_temp);
    let sign = matches!(
        op,
        IrBinaryOp::SDiv | IrBinaryOp::SRem | IrBinaryOp::AShr
            | IrBinaryOp::Slt | IrBinaryOp::Sle | IrBinaryOp::Sgt | IrBinaryOp::Sge
    );

    let (a, a_insts) = get_i64_quad(mgr, naming, lhs, sign);
    insts.extend(a_insts);
    for r in a {
        mgr.pin_register(r);
    }

    let is_cmp = matches!(
        op,
        IrBinaryOp::Eq | IrBinaryOp::Ne
            | IrBinaryOp::Slt | IrBinaryOp::Sle | IrBinaryOp::Sgt | IrBinaryOp::Sge
            | IrBinaryOp::Ult | IrBinaryOp::Ule | IrBinaryOp::Ugt | IrBinaryOp::Uge
    );
    let is_shift = matches!(op, IrBinaryOp::Shl | IrBinaryOp::LShr | IrBinaryOp::AShr);

    let b = if is_shift {
        [Reg::R0; 4]
    } else {
        let (bq, bi) = get_i64_quad(mgr, naming, rhs, sign);
        insts.extend(bi);
        for r in bq {
            mgr.pin_register(r);
        }
        bq
    };

    // Park add/sub/bitwise operands in ABI regs before allocating the 4-word
    // result, so we never hold 4+4+4 T/S pins at once.
    let parked = matches!(
        op,
        IrBinaryOp::Add | IrBinaryOp::Sub | IrBinaryOp::And | IrBinaryOp::Or | IrBinaryOp::Xor
    );
    if parked {
        insts.extend(park_quad(I64_PARK_A, a));
        insts.extend(park_quad(I64_PARK_B, b));
        unpin_quad(mgr, a);
        unpin_quad(mgr, b);
    }

    let result_lo = mgr.get_register(result_name.clone());
    insts.extend(mgr.take_instructions());
    mgr.pin_register(result_lo);

    if is_cmp {
        match op {
            IrBinaryOp::Eq => {
                let x = new_temp(mgr, naming, &mut insts, "eq64", "x");
                em3(mgr, &mut insts, Src::N(&x), Src::R(a[0]), Src::R(b[0]), AsmInst::Xor);
                for i in 1..4 {
                    let t = new_temp(mgr, naming, &mut insts, "eq64", &format!("x{i}"));
                    em3(mgr, &mut insts, Src::N(&t), Src::R(a[i]), Src::R(b[i]), AsmInst::Xor);
                    em3(mgr, &mut insts, Src::N(&x), Src::N(&x), Src::N(&t), AsmInst::Or);
                }
                let one = new_temp(mgr, naming, &mut insts, "eq64", "one");
                em_li(mgr, &mut insts, Src::N(&one), 1);
                em3(mgr, &mut insts, Src::R(result_lo), Src::N(&x), Src::N(&one), AsmInst::Sltu);
            }
            IrBinaryOp::Ne => {
                let x = new_temp(mgr, naming, &mut insts, "ne64", "x");
                em3(mgr, &mut insts, Src::N(&x), Src::R(a[0]), Src::R(b[0]), AsmInst::Xor);
                for i in 1..4 {
                    let t = new_temp(mgr, naming, &mut insts, "ne64", &format!("x{i}"));
                    em3(mgr, &mut insts, Src::N(&t), Src::R(a[i]), Src::R(b[i]), AsmInst::Xor);
                    em3(mgr, &mut insts, Src::N(&x), Src::N(&x), Src::N(&t), AsmInst::Or);
                }
                em3(mgr, &mut insts, Src::R(result_lo), Src::R(Reg::R0), Src::N(&x), AsmInst::Sltu);
            }
            IrBinaryOp::Ult => insts.extend(emit_i64_ult(result_lo, a, b, naming)),
            IrBinaryOp::Ugt => insts.extend(emit_i64_ult(result_lo, b, a, naming)),
            IrBinaryOp::Ule => {
                insts.extend(emit_i64_ult(result_lo, b, a, naming));
                insts.extend(invert01(mgr, naming, result_lo, result_lo));
            }
            IrBinaryOp::Uge => {
                insts.extend(emit_i64_ult(result_lo, a, b, naming));
                insts.extend(invert01(mgr, naming, result_lo, result_lo));
            }
            IrBinaryOp::Slt => insts.extend(emit_i64_slt(result_lo, a, b, naming)),
            IrBinaryOp::Sgt => insts.extend(emit_i64_slt(result_lo, b, a, naming)),
            IrBinaryOp::Sle => {
                insts.extend(emit_i64_slt(result_lo, b, a, naming));
                insts.extend(invert01(mgr, naming, result_lo, result_lo));
            }
            IrBinaryOp::Sge => {
                insts.extend(emit_i64_slt(result_lo, a, b, naming));
                insts.extend(invert01(mgr, naming, result_lo, result_lo));
            }
            _ => unreachable!(),
        }
        mgr.bind_value_to_register(result_name, result_lo);
        mgr.unpin_register(result_lo);
        for r in a {
            mgr.unpin_register(r);
        }
        if !is_shift {
            for r in b {
                mgr.unpin_register(r);
            }
        }
        return insts;
    }

    let mut result = [result_lo, Reg::R0, Reg::R0, Reg::R0];
    let result_names = [
        result_name.clone(),
        naming.i64_word_name(&result_name, 1),
        naming.i64_word_name(&result_name, 2),
        naming.i64_word_name(&result_name, 3),
    ];
    for i in 1..4 {
        result[i] = mgr.get_register(result_names[i].clone());
        insts.extend(mgr.take_instructions());
        mgr.pin_register(result[i]);
    }

    match op {
        IrBinaryOp::Add => {
            insts.extend(emit_i64_add(result, I64_PARK_A, I64_PARK_B));
        }
        IrBinaryOp::Sub => {
            insts.extend(emit_i64_sub(result, I64_PARK_A, I64_PARK_B));
        }
        IrBinaryOp::And => insts.extend(emit_i64_bitwise(AsmInst::And, result, I64_PARK_A, I64_PARK_B)),
        IrBinaryOp::Or => insts.extend(emit_i64_bitwise(AsmInst::Or, result, I64_PARK_A, I64_PARK_B)),
        IrBinaryOp::Xor => insts.extend(emit_i64_bitwise(AsmInst::Xor, result, I64_PARK_A, I64_PARK_B)),
        IrBinaryOp::Mul => {
            for r in result {
                mgr.unpin_register(r);
            }
            insts.extend(emit_i64_mul(&result_names, a, b, mgr, naming));
            for i in 0..4 {
                result[i] = mgr.get_register(result_names[i].clone());
                insts.extend(mgr.take_instructions());
                mgr.pin_register(result[i]);
            }
        }
        IrBinaryOp::UDiv | IrBinaryOp::URem | IrBinaryOp::SDiv | IrBinaryOp::SRem => {
            for r in result {
                mgr.unpin_register(r);
            }
            unpin_quad(mgr, a);
            unpin_quad(mgr, b);
            let want_rem = matches!(op, IrBinaryOp::URem | IrBinaryOp::SRem);
            let signed = matches!(op, IrBinaryOp::SDiv | IrBinaryOp::SRem);
            if signed {
                insts.extend(emit_i64_sdiv(a, b, want_rem, mgr, naming, &result_names));
            } else {
                insts.extend(emit_i64_udiv(a, b, want_rem, mgr, naming, &result_names));
            }
            for i in 0..4 {
                result[i] = mgr.get_register(result_names[i].clone());
                insts.extend(mgr.take_instructions());
                mgr.pin_register(result[i]);
            }
        }
        IrBinaryOp::Shl | IrBinaryOp::LShr | IrBinaryOp::AShr => {
            let shl = matches!(op, IrBinaryOp::Shl);
            let arithmetic = matches!(op, IrBinaryOp::AShr);
            match rhs {
                Value::Constant(c) => {
                    insts.extend(park_quad(I64_PARK_A, a));
                    unpin_quad(mgr, a);
                    let n = (*c as u64) & 63;
                    insts.extend(emit_i64_shift_const(
                        result, I64_PARK_A, n as u32, shl, arithmetic, naming,
                    ));
                }
                _ => {
                    let (amt, ai) = shift_amt_reg(mgr, naming, rhs);
                    insts.extend(ai);
                    mgr.pin_register(amt);
                    insts.push(AsmInst::Add(I64_CY, amt, Reg::R0));
                    insts.push(AsmInst::Li(I64_TMP, 63));
                    insts.push(AsmInst::And(I64_CY, I64_CY, I64_TMP));
                    insts.extend(park_quad(I64_PARK_A, a));
                    unpin_quad(mgr, a);
                    mgr.unpin_register(amt);
                    insts.extend(emit_i64_shift_var(
                        result, I64_PARK_A, I64_CY, shl, arithmetic, naming,
                    ));
                }
            }
        }
        _ => panic!("unexpected I64 binary op {op:?}"),
    }

    bind_i64(mgr, naming, &result_name, result);
    for r in result {
        mgr.unpin_register(r);
    }
    if !parked {
        for r in a {
            mgr.unpin_register(r);
        }
        if !is_shift {
            for r in b {
                mgr.unpin_register(r);
            }
        }
    }
    insts
}

pub fn lower_i64_extend(
    mgr: &mut RegisterPressureManager,
    naming: &mut NameGenerator,
    op: IrUnaryOp,
    operand: &Value,
    result_temp: TempId,
) -> Vec<AsmInst> {
    let mut insts = Vec::new();
    let result_name = naming.temp_name(result_temp);
    let sign = matches!(op, IrUnaryOp::SExt);
    let (quad, ext) = get_i64_quad(mgr, naming, operand, sign);
    insts.extend(ext);
    let mut result = [Reg::R0; 4];
    result[0] = mgr.get_register(result_name.clone());
    insts.extend(mgr.take_instructions());
    if result[0] != quad[0] {
        insts.push(AsmInst::Add(result[0], quad[0], Reg::R0));
    }
    mgr.pin_register(result[0]);
    for i in 1..4 {
        let name = naming.i64_word_name(&result_name, i as u8);
        result[i] = mgr.get_register(name);
        insts.extend(mgr.take_instructions());
        if result[i] != quad[i] {
            insts.push(AsmInst::Add(result[i], quad[i], Reg::R0));
        }
    }
    mgr.unpin_register(result[0]);
    bind_i64(mgr, naming, &result_name, result);
    insts
}

pub fn lower_trunc_i64_to_i32(
    mgr: &mut RegisterPressureManager,
    naming: &mut NameGenerator,
    operand: &Value,
    result_temp: TempId,
) -> Vec<AsmInst> {
    let mut insts = Vec::new();
    let result_name = naming.temp_name(result_temp);
    let (quad, ext) = get_i64_quad(mgr, naming, operand, true);
    insts.extend(ext);
    let lo = mgr.get_register(result_name.clone());
    insts.extend(mgr.take_instructions());
    if lo != quad[0] {
        insts.push(AsmInst::Add(lo, quad[0], Reg::R0));
    }
    mgr.pin_register(lo);
    let hi_name = naming.i32_high_name(&result_name);
    let hi = mgr.get_register(hi_name);
    insts.extend(mgr.take_instructions());
    if hi != quad[1] {
        insts.push(AsmInst::Add(hi, quad[1], Reg::R0));
    }
    mgr.unpin_register(lo);
    crate::instr::wide::bind_i32(mgr, naming, &result_name, lo, hi);
    insts
}
