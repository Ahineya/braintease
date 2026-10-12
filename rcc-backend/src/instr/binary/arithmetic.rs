//! Arithmetic and logical operation generation

use rcc_frontend::ir::IrBinaryOp;
use rcc_codegen::{AsmInst, Reg};
use crate::naming::NameGenerator;
use crate::regmgmt::RegisterPressureManager;

/// Signed operations synthesized from the existing 16-bit ISA. Inputs and
/// result are pinned by the caller; only the scratch registers are released.
pub(super) fn generate_signed_arithmetic_instructions(
    mgr: &mut RegisterPressureManager,
    naming: &mut NameGenerator,
    op: IrBinaryOp,
    result: Reg,
    lhs: Reg,
    rhs: Reg,
) -> Vec<AsmInst> {
    let mut insts = Vec::new();
    let sign = mgr.get_register(naming.temp_with_context("signed", "sign"));
    insts.extend(mgr.take_instructions());
    mgr.pin_register(sign);
    insts.push(AsmInst::Slt(sign, lhs, Reg::R0));
    insts.push(AsmInst::Sub(sign, Reg::R0, sign));

    if op == IrBinaryOp::AShr {
        // For a negative x, ~((~x) >> n) fills the vacated bits with ones.
        // This also handles a zero count without shifting by the word width.
        insts.push(AsmInst::Xor(result, lhs, sign));
        insts.push(AsmInst::Srl(result, result, rhs));
        insts.push(AsmInst::Xor(result, result, sign));
    } else {
        assert!(matches!(op, IrBinaryOp::SDiv | IrBinaryOp::SRem));
        let rhs_sign = mgr.get_register(naming.temp_with_context("signed", "rhs_sign"));
        insts.extend(mgr.take_instructions());
        mgr.pin_register(rhs_sign);
        let rhs_abs = mgr.get_register(naming.temp_with_context("signed", "rhs_abs"));
        insts.extend(mgr.take_instructions());
        mgr.pin_register(rhs_abs);

        insts.push(AsmInst::Slt(rhs_sign, rhs, Reg::R0));
        insts.push(AsmInst::Sub(rhs_sign, Reg::R0, rhs_sign));
        insts.push(AsmInst::Xor(rhs_abs, rhs, rhs_sign));
        insts.push(AsmInst::Sub(rhs_abs, rhs_abs, rhs_sign));
        // Magnitudes are unsigned, so abs(INT_MIN) is representable as 32768.
        insts.push(AsmInst::Xor(result, lhs, sign));
        insts.push(AsmInst::Sub(result, result, sign));
        if op == IrBinaryOp::SDiv {
            insts.push(AsmInst::Xor(sign, sign, rhs_sign));
            insts.push(AsmInst::Div(result, result, rhs_abs));
        } else {
            // C remainder has the sign of the dividend, independent of rhs.
            insts.push(AsmInst::Mod(result, result, rhs_abs));
        }
        insts.push(AsmInst::Xor(result, result, sign));
        insts.push(AsmInst::Sub(result, result, sign));

        mgr.unpin_register(rhs_sign);
        mgr.free_register(rhs_sign);
        mgr.unpin_register(rhs_abs);
        mgr.free_register(rhs_abs);
    }
    mgr.unpin_register(sign);
    mgr.free_register(sign);
    insts
}

/// Generate arithmetic instruction for the given operation
/// 
/// Returns None if the operation is not an arithmetic operation
/// (i.e., it's a comparison operation that needs special handling)
pub(super) fn generate_arithmetic_instruction(
    op: IrBinaryOp,
    result_reg: Reg,
    lhs_reg: Reg,
    rhs_reg: Reg,
    should_swap: bool,
) -> Option<AsmInst> {
    match op {
        IrBinaryOp::Add => {
            // Commutative - use the registers as evaluated
            Some(if should_swap {
                AsmInst::Add(result_reg, rhs_reg, lhs_reg)
            } else {
                AsmInst::Add(result_reg, lhs_reg, rhs_reg)
            })
        }
        IrBinaryOp::Sub => {
            // Non-commutative - always use original order
            Some(AsmInst::Sub(result_reg, lhs_reg, rhs_reg))
        }
        IrBinaryOp::Mul => {
            // Commutative - use the registers as evaluated
            Some(if should_swap {
                AsmInst::Mul(result_reg, rhs_reg, lhs_reg)
            } else {
                AsmInst::Mul(result_reg, lhs_reg, rhs_reg)
            })
        }
        IrBinaryOp::UDiv => {
            // VM DIV is unsigned - direct mapping
            Some(AsmInst::Div(result_reg, lhs_reg, rhs_reg))
        }
        IrBinaryOp::URem => {
            // VM MOD is unsigned - direct mapping
            Some(AsmInst::Mod(result_reg, lhs_reg, rhs_reg))
        }
        IrBinaryOp::And => {
            // Commutative
            Some(if should_swap {
                AsmInst::And(result_reg, rhs_reg, lhs_reg)
            } else {
                AsmInst::And(result_reg, lhs_reg, rhs_reg)
            })
        }
        IrBinaryOp::Or => {
            // Commutative
            Some(if should_swap {
                AsmInst::Or(result_reg, rhs_reg, lhs_reg)
            } else {
                AsmInst::Or(result_reg, lhs_reg, rhs_reg)
            })
        }
        IrBinaryOp::Xor => {
            // Commutative
            Some(if should_swap {
                AsmInst::Xor(result_reg, rhs_reg, lhs_reg)
            } else {
                AsmInst::Xor(result_reg, lhs_reg, rhs_reg)
            })
        }
        IrBinaryOp::Shl => {
            // Non-commutative
            Some(AsmInst::Sll(result_reg, lhs_reg, rhs_reg))
        }
        IrBinaryOp::LShr => {
            Some(AsmInst::Srl(result_reg, lhs_reg, rhs_reg))
        }
        // Comparisons and signed multi-instruction operations handled elsewhere
        _ => None,
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::instr::binary::{lower_binary_op, lower_binary_op_immediate};
    use crate::naming::new_function_naming;
    use rcc_frontend::ir::Value;

    /// Execute the existing word instructions emitted by these operations.
    /// Expected results below use host signed arithmetic, independently of the
    /// magnitude/sign-mask implementation in the compiler.
    fn execute(insts: &[AsmInst], mut regs: [u16; 32], result: Reg) -> i16 {
        for inst in insts {
            let (rd, value) = match *inst {
                AsmInst::Li(rd, imm) => (rd, imm as u16),
                AsmInst::Slt(rd, lhs, rhs) => (rd, ((regs[lhs as usize] as i16) < (regs[rhs as usize] as i16)) as u16),
                AsmInst::Sub(rd, lhs, rhs) => (rd, regs[lhs as usize].wrapping_sub(regs[rhs as usize])),
                AsmInst::Xor(rd, lhs, rhs) => (rd, regs[lhs as usize] ^ regs[rhs as usize]),
                AsmInst::Div(rd, lhs, rhs) => (rd, regs[lhs as usize] / regs[rhs as usize]),
                AsmInst::Mod(rd, lhs, rhs) => (rd, regs[lhs as usize] % regs[rhs as usize]),
                AsmInst::Srl(rd, lhs, rhs) => (rd, regs[lhs as usize] >> (regs[rhs as usize] & 15)),
                _ => panic!("Unexpected signed arithmetic instruction: {inst:?}"),
            };
            regs[rd as usize] = value;
        }
        regs[result as usize] as i16
    }

    fn sequence(op: IrBinaryOp, result: Reg) -> Vec<AsmInst> {
        let mut mgr = RegisterPressureManager::new(0);
        mgr.init();
        mgr.take_instructions();
        let mut naming = new_function_naming();
        mgr.pin_register(Reg::A0);
        mgr.pin_register(Reg::A1);
        mgr.pin_register(result);
        generate_signed_arithmetic_instructions(&mut mgr, &mut naming, op, result, Reg::A0, Reg::A1)
    }

    #[test]
    fn signed_div_rem_cover_every_dividend_and_boundary_divisors() {
        for op in [IrBinaryOp::SDiv, IrBinaryOp::SRem] {
            for result in [Reg::A0, Reg::A2] {
                let insts = sequence(op, result);
                for lhs in i16::MIN..=i16::MAX {
                    for rhs in [i16::MIN, -32767, -257, -3, -1, 1, 3, 257, i16::MAX] {
                        // Signed overflow and division by zero are undefined C.
                        if lhs == i16::MIN && rhs == -1 { continue; }
                        let mut regs = [0; 32];
                        regs[Reg::A0 as usize] = lhs as u16;
                        regs[Reg::A1 as usize] = rhs as u16;
                        let expected = if op == IrBinaryOp::SDiv { lhs / rhs } else { lhs % rhs };
                        assert_eq!(execute(&insts, regs, result), expected, "{op:?}: {lhs}, {rhs}");
                    }
                }
            }
        }
    }

    #[test]
    fn signed_right_shift_covers_every_word_and_valid_count() {
        for result in [Reg::A0, Reg::A2] {
            let insts = sequence(IrBinaryOp::AShr, result);
            for lhs in i16::MIN..=i16::MAX {
                for rhs in 0..16 {
                    let mut regs = [0; 32];
                    regs[Reg::A0 as usize] = lhs as u16;
                    regs[Reg::A1 as usize] = rhs;
                    assert_eq!(execute(&insts, regs, result), lhs >> rhs, "{lhs} >> {rhs}");
                }
            }
        }
    }

    #[test]
    fn signed_immediate_and_general_lowering_agree() {
        for op in [IrBinaryOp::SDiv, IrBinaryOp::SRem, IrBinaryOp::AShr] {
            for lhs in [i16::MIN, -32767, -7, -1, 0, 1, 7, i16::MAX] {
                let divisors: &[i16] = if op == IrBinaryOp::AShr { &[0, 1, 7, 15] } else { &[-257, -3, 1, 3, 257] };
                for &rhs in divisors {
                    for immediate in [false, true] {
                        let mut mgr = RegisterPressureManager::new(0);
                        mgr.init();
                        mgr.take_instructions();
                        let mut naming = new_function_naming();
                        let lhs_val = Value::Constant(lhs as i64);
                        let insts = if immediate {
                            lower_binary_op_immediate(&mut mgr, &mut naming, op, &lhs_val, rhs, 1)
                        } else {
                            lower_binary_op(&mut mgr, &mut naming, op, &lhs_val, &Value::Constant(rhs as i64), 1)
                        };
                        let result = mgr.get_register(naming.temp_name(1));
                        assert!(mgr.take_instructions().is_empty(), "Result must remain in its register");
                        let expected = match op {
                            IrBinaryOp::SDiv => lhs / rhs,
                            IrBinaryOp::SRem => lhs % rhs,
                            IrBinaryOp::AShr => lhs >> rhs,
                            _ => unreachable!(),
                        };
                        assert_eq!(execute(&insts, [0; 32], result), expected, "{op:?}: {lhs}, {rhs}, immediate={immediate}");
                    }
                }
            }
        }
    }
}
