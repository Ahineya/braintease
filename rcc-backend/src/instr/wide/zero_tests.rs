use super::*;
use crate::instr::tests::machine::Machine;
use crate::naming::new_function_naming;

fn pair(mgr: &mut RegisterPressureManager, naming: &NameGenerator, id: TempId,
    value: u32, setup: &mut Vec<AsmInst>) {
    let name = naming.temp_name(id);
    let lo = mgr.get_register(name.clone());
    setup.extend(mgr.take_instructions());
    setup.push(AsmInst::Li(lo, value as i16));
    mgr.pin_register(lo);
    let hi = mgr.get_register(naming.i32_high_name(&name));
    setup.extend(mgr.take_instructions());
    setup.push(AsmInst::Li(hi, (value >> 16) as i16));
    bind_i32(mgr, naming, &name, lo, hi);
    mgr.unpin_register(lo);
}

#[test]
fn wide_zero_comparisons_need_no_bitwise_conversion() {
    for op in [IrBinaryOp::Eq, IrBinaryOp::Ne] {
        for zero_left in [false, true] {
            let mut mgr = RegisterPressureManager::new(0);
            mgr.init();
            let mut naming = new_function_naming();
            let mut setup = Vec::new();
            pair(&mut mgr, &naming, 1, 0x0001ffff, &mut setup);
            let (lhs, rhs) = if zero_left {
                (Value::Constant(0), Value::Temp(1))
            } else { (Value::Temp(1), Value::Constant(0)) };
            let code = lower_i32_binary(&mut mgr, &mut naming, op, &lhs, &rhs, 2);
            assert!(!code.iter().any(|i| matches!(i, AsmInst::Xor(..) | AsmInst::Or(..))),
                "wide zero comparison must not convert words to bits: {code:#?}");
        }
    }
}

#[test]
fn wide_truthiness_needs_no_bitwise_conversion() {
    let mut mgr = RegisterPressureManager::new(0);
    mgr.init();
    let mut naming = new_function_naming();
    let mut setup = Vec::new();
    pair(&mut mgr, &naming, 1, 0x0001ffff, &mut setup);
    let (_, code) = i32_to_cond_reg(&mut mgr, &mut naming, &Value::Temp(1));
    assert!(!code.iter().any(|i| matches!(i, AsmInst::Xor(..) | AsmInst::Or(..))),
        "wide truthiness must not convert words to bits: {code:#?}");
}

#[test]
fn wide_equality_compares_both_words_without_summing_differences() {
    let values = [0u32, 1, 0xffff, 0x10000, 0x1ffff, 0x80000000, 0xffffffff];
    for a in values {
        for b in values {
            for op in [IrBinaryOp::Eq, IrBinaryOp::Ne] {
                let mut mgr = RegisterPressureManager::new(0);
                mgr.init();
                let mut naming = new_function_naming();
                let mut code = Vec::new();
                pair(&mut mgr, &naming, 1, a, &mut code);
                pair(&mut mgr, &naming, 2, b, &mut code);
                let comparison = lower_i32_binary(&mut mgr, &mut naming, op,
                    &Value::Temp(1), &Value::Temp(2), 3);
                assert!(!comparison.iter().any(|i| matches!(i, AsmInst::Or(..) | AsmInst::Xor(..))));
                assert_eq!(comparison.iter().filter(|i| !matches!(i, AsmInst::Label(_) | AsmInst::Comment(_))).count(), 4);
                code.extend(comparison);
                let result = mgr.get_register(naming.temp_name(3));
                code.extend(mgr.take_instructions());
                code.push(AsmInst::Move(Reg::Rv0, result));
                let mut m = Machine::new(64000);
                m.run(&code);
                assert_eq!(m.regs[Reg::Rv0 as usize], u16::from(if op == IrBinaryOp::Eq { a == b } else { a != b }));
            }
        }
    }
}

#[test]
fn wide_truthiness_checks_every_i64_word_and_preserves_sources() {
    for value in [0u64, 1, 1 << 16, 1 << 32, 1 << 48, 1 << 63, 0x1ffff, u64::MAX] {
        let mut mgr = RegisterPressureManager::new(0);
        mgr.init();
        let mut naming = new_function_naming();
        let mut code = Vec::new();
        for n in 0..12 {
            let r = mgr.get_register(format!("pressure_{n}"));
            code.extend(mgr.take_instructions());
            code.push(AsmInst::Li(r, n));
        }
        let base = naming.temp_name(1);
        let names = [base.clone(), format!("{base}_w1"), format!("{base}_w2"), format!("{base}_w3")];
        for (i, name) in names.iter().enumerate() {
            let r = mgr.get_register(name.clone());
            code.extend(mgr.take_instructions());
            code.push(AsmInst::Li(r, (value >> (i * 16)) as i16));
        }
        mgr.set_i64_words(base, [names[1].clone(), names[2].clone(), names[3].clone()]);
        let (result, test) = i32_to_cond_reg(&mut mgr, &mut naming, &Value::Temp(1));
        assert!(!test.iter().any(|i| matches!(i, AsmInst::Or(..) | AsmInst::Xor(..))));
        code.extend(test);
        code.push(AsmInst::Move(Reg::Rv0, result));
        for (i, name) in names.iter().enumerate() {
            let r = mgr.get_register(name.clone());
            code.extend(mgr.take_instructions());
            code.push(AsmInst::Move([Reg::A0, Reg::A1, Reg::A2, Reg::A3][i], r));
        }
        let mut m = Machine::new(64000);
        for offset in 0..1024 { m.memory.insert((2, offset), 0xabcd); }
        m.run(&code);
        assert_eq!(m.regs[Reg::Rv0 as usize], u16::from(value != 0));
        for (i, r) in [Reg::A0, Reg::A1, Reg::A2, Reg::A3].iter().enumerate() {
            assert_eq!(m.regs[*r as usize], (value >> (i * 16)) as u16);
        }
    }
}

#[test]
fn wide_zero_tests_preserve_live_pairs_under_register_pressure() {
    for value in [0u32, 1, 0xffff, 0x10000, 0x1ffff, 0x7fffffff, 0x80000000, 0xffffffff] {
        for op in [Some(IrBinaryOp::Eq), Some(IrBinaryOp::Ne), None] {
            for zero_left in [false, true] {
                let mut mgr = RegisterPressureManager::new(0);
                mgr.init();
                let mut naming = new_function_naming();
                let mut code = Vec::new();
                for n in 0..12 {
                    let r = mgr.get_register(format!("pressure_{n}"));
                    code.extend(mgr.take_instructions());
                    code.push(AsmInst::Li(r, n));
                }
                pair(&mut mgr, &naming, 1, 0x12345678, &mut code);
                pair(&mut mgr, &naming, 2, value, &mut code);
                let result = if let Some(op) = op {
                    let (lhs, rhs) = if zero_left { (Value::Constant(0), Value::Temp(2)) }
                        else { (Value::Temp(2), Value::Constant(0)) };
                    code.extend(lower_i32_binary(&mut mgr, &mut naming, op, &lhs, &rhs, 3));
                    let r = mgr.get_register(naming.temp_name(3));
                    code.extend(mgr.take_instructions());
                    r
                } else {
                    let (r, test) = i32_to_cond_reg(&mut mgr, &mut naming, &Value::Temp(2));
                    code.extend(test);
                    r
                };
                code.push(AsmInst::Move(Reg::Rv0, result));
                let (lo, hi, restore) = get_i32_pair(&mut mgr, &mut naming, &Value::Temp(1), false);
                code.extend(restore);
                code.push(AsmInst::Move(Reg::A0, lo));
                code.push(AsmInst::Move(Reg::A1, hi));
                let (lo, hi, restore) = get_i32_pair(&mut mgr, &mut naming, &Value::Temp(2), false);
                code.extend(restore);
                code.push(AsmInst::Move(Reg::A2, lo));
                code.push(AsmInst::Move(Reg::A3, hi));
                let mut m = Machine::new(64000);
                for offset in 0..1024 { m.memory.insert((2, offset), 0xabcd); }
                m.run(&code);
                let expected = if op == Some(IrBinaryOp::Eq) { value == 0 } else { value != 0 };
                assert_eq!(m.regs[Reg::Rv0 as usize] != 0, expected, "{value:x} {op:?}");
                if op.is_some() { assert!(m.regs[Reg::Rv0 as usize] <= 1); }
                assert_eq!((m.regs[Reg::A0 as usize], m.regs[Reg::A1 as usize]), (0x5678, 0x1234));
                assert_eq!((m.regs[Reg::A2 as usize], m.regs[Reg::A3 as usize]), (value as u16, (value >> 16) as u16));
            }
        }
    }
}
