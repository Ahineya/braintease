//! Finalize frames from the emitted body, after spills and calls are known.
//! Framed functions keep the seven-cell ABI header so stack arguments and
//! varargs remain compatible with independently compiled callers/callees.
use rcc_codegen::{AsmInst, Reg};
use std::collections::BTreeSet;

pub(crate) fn finalize(mut code: Vec<AsmInst>, frame_top: i16) -> Vec<AsmInst> {
    let Some(reserve) = code.iter().position(|i| matches!(i, AsmInst::Comment(s) if s.starts_with("Reserve ") && s.ends_with(" spill slots above locals"))) else {
        return code;
    };
    let Some(epilogue) = code
        .iter()
        .position(|i| matches!(i, AsmInst::Comment(s) if s == "=== Function Epilogue ==="))
    else {
        return code;
    };
    let body = &code[reserve + 2..epilogue];
    // Raw assembly may contain calls, register writes, or hidden frame uses.
    // Retain the original complete preservation until its effects are known.
    if body
        .iter()
        .any(|i| matches!(i, AsmInst::Raw(_) | AsmInst::Push(_) | AsmInst::Pop(_)))
    {
        remove_fallthrough_jumps(&mut code);
        return code;
    }
    let mut writes = BTreeSet::new();
    let mut needs_frame = false;
    for instruction in body {
        use AsmInst::*;
        let registers: Vec<Reg> = match instruction {
            Add(a, b, c)
            | Sub(a, b, c)
            | Mul(a, b, c)
            | Div(a, b, c)
            | Mod(a, b, c)
            | And(a, b, c)
            | Or(a, b, c)
            | Xor(a, b, c)
            | Sll(a, b, c)
            | Srl(a, b, c)
            | Slt(a, b, c)
            | Sltu(a, b, c)
            | Load(a, b, c) => {
                writes.insert(*a);
                vec![*a, *b, *c]
            }
            Store(a, b, c) => vec![*a, *b, *c],
            AddI(a, b, _)
            | SubI(a, b, _)
            | MulI(a, b, _)
            | DivI(a, b, _)
            | ModI(a, b, _)
            | Move(a, b) => {
                writes.insert(*a);
                vec![*a, *b]
            }
            Li(a, _) | LiLabel(a, _) | Inc(a) | Dec(a) => {
                writes.insert(*a);
                vec![*a]
            }
            Beq(a, b, _) | Bne(a, b, _) | Blt(a, b, _) | Bge(a, b, _) => vec![*a, *b],
            Call(_) | Jal(..) => {
                writes.extend([Reg::Ra, Reg::Rab]);
                Vec::new()
            }
            Jalr(link, bank, addr) => {
                if *link != Reg::R0 {
                    writes.extend([*link, Reg::Rab]);
                }
                vec![*bank, *addr]
            }
            _ => Vec::new(),
        };
        needs_frame |= registers.iter().any(|r| matches!(r, Reg::Fp | Reg::Sp));
    }
    let saved: Vec<_> = [Reg::Ra, Reg::Rab, Reg::S0, Reg::S1, Reg::S2, Reg::S3]
        .into_iter()
        .filter(|r| writes.contains(r))
        .collect();
    needs_frame |= !saved.is_empty();
    let body = body.to_vec();
    code.clear();
    code.push(AsmInst::Comment("=== Function Prologue ===".into()));
    if needs_frame {
        // Skip unused ABI slots in one step; only actual modified registers
        // incur memory traffic. FP always occupies the original third slot.
        let mut position = 0;
        for (slot, register) in [
            Reg::Ra,
            Reg::Rab,
            Reg::Fp,
            Reg::S0,
            Reg::S1,
            Reg::S2,
            Reg::S3,
        ]
        .into_iter()
        .enumerate()
        {
            if register != Reg::Fp && !saved.contains(&register) {
                continue;
            }
            let slot = slot as i16;
            if slot != position {
                code.push(AsmInst::AddI(Reg::Sp, Reg::Sp, slot - position));
            }
            code.push(AsmInst::Store(register, Reg::Sb, Reg::Sp));
            position = slot;
        }
        code.push(AsmInst::AddI(Reg::Sp, Reg::Sp, 7 - position));
        code.push(AsmInst::Move(Reg::Fp, Reg::Sp));
        if frame_top != 0 {
            code.push(AsmInst::AddI(Reg::Sp, Reg::Sp, frame_top));
        }
    }
    code.extend(body);
    code.push(AsmInst::Comment("=== Function Epilogue ===".into()));
    if needs_frame {
        for (offset, register) in [
            (-1, Reg::S3),
            (-2, Reg::S2),
            (-3, Reg::S1),
            (-4, Reg::S0),
            (-6, Reg::Rab),
            (-7, Reg::Ra),
        ] {
            if saved.contains(&register) {
                code.push(AsmInst::AddI(Reg::Sc, Reg::Fp, offset));
                code.push(AsmInst::Load(register, Reg::Sb, Reg::Sc));
            }
        }
        code.push(AsmInst::AddI(Reg::Sp, Reg::Fp, -7));
        code.push(AsmInst::AddI(Reg::Sc, Reg::Fp, -5));
        code.push(AsmInst::Load(Reg::Fp, Reg::Sb, Reg::Sc));
    }
    code.push(AsmInst::Jalr(Reg::R0, Reg::Rab, Reg::Ra));
    remove_fallthrough_jumps(&mut code);
    code
}

fn remove_fallthrough_jumps(code: &mut Vec<AsmInst>) {
    let mut remove = BTreeSet::new();
    for (index, instruction) in code.iter().enumerate() {
        let AsmInst::Beq(Reg::R0, Reg::R0, target) = instruction else {
            continue;
        };
        for next in &code[index + 1..] {
            match next {
                AsmInst::Comment(_) => {}
                AsmInst::Label(label) if label == target => {
                    remove.insert(index);
                    break;
                }
                AsmInst::Label(_) => {}
                _ => break,
            }
        }
    }
    let mut index = 0;
    code.retain(|_| {
        let keep = !remove.contains(&index);
        index += 1;
        keep
    });
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::instr::tests::machine::Machine;

    fn frame(body: Vec<AsmInst>) -> Vec<AsmInst> {
        let mut builder = super::super::FunctionBuilder::new();
        builder
            .begin_function(0)
            .add_instructions(body)
            .end_function(None);
        finalize(builder.build(), 0)
    }

    #[test]
    fn leaf_preserves_caller_registers_without_stack_traffic() {
        let code = frame(vec![AsmInst::Add(Reg::Rv0, Reg::A0, Reg::A1)]);
        assert!(!code
            .iter()
            .any(|i| matches!(i, AsmInst::Load(..) | AsmInst::Store(..))));
        let mut machine = Machine::new(64000);
        machine.regs[Reg::Sp as usize] = 1000;
        machine.regs[Reg::Fp as usize] = 700;
        machine.regs[Reg::A0 as usize] = 65530;
        machine.regs[Reg::A1 as usize] = 13;
        machine.run(&code[..code.len() - 1]);
        assert_eq!(machine.regs[Reg::Rv0 as usize], 7);
        assert_eq!(machine.regs[Reg::Sp as usize], 1000);
        assert_eq!(machine.regs[Reg::Fp as usize], 700);
    }

    #[test]
    fn selective_frame_preserves_modified_saved_register_and_stack_abi() {
        let code = frame(vec![
            AsmInst::Li(Reg::S1, 77),
            AsmInst::AddI(Reg::Sc, Reg::Fp, -8),
            AsmInst::Load(Reg::Rv0, Reg::Sb, Reg::Sc),
        ]);
        assert!(!code.iter().any(|i| matches!(
            i,
            AsmInst::Store(Reg::Ra | Reg::Rab | Reg::S0 | Reg::S2 | Reg::S3, ..)
        )));
        let mut machine = Machine::new(64000);
        machine.regs[Reg::Sp as usize] = 1000;
        machine.regs[Reg::Fp as usize] = 700;
        machine.regs[Reg::S1 as usize] = 321;
        machine.memory.insert((2, 999), 456);
        machine.run(&code[..code.len() - 1]);
        assert_eq!(machine.regs[Reg::Rv0 as usize], 456);
        assert_eq!(machine.regs[Reg::S1 as usize], 321);
        assert_eq!(machine.regs[Reg::Sp as usize], 1000);
        assert_eq!(machine.regs[Reg::Fp as usize], 700);
    }

    #[test]
    fn opaque_assembly_retains_full_preservation() {
        let code = frame(vec![AsmInst::Raw("CALL opaque".into())]);
        for reg in [
            Reg::Ra,
            Reg::Rab,
            Reg::Fp,
            Reg::S0,
            Reg::S1,
            Reg::S2,
            Reg::S3,
        ] {
            assert!(code
                .iter()
                .any(|i| matches!(i,AsmInst::Store(r,..) if *r==reg)));
        }
    }
}
