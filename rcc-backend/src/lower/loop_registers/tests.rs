use super::*;
use crate::instr::tests::machine::Machine;
use rcc_frontend::ir::{BasicBlock, IrType};
fn function() -> Function {
    Function::new("loop".into(), IrType::I16)
}
fn framed(body: Vec<A>) -> Vec<A> {
    let mut builder = crate::function::FunctionBuilder::new();
    builder
        .begin_function(0)
        .add_instructions(body)
        .end_function(None);
    builder.build()
}
fn source() -> Vec<A> {
    vec![
        A::AddI(Reg::Sc, Reg::Fp, 0),
        A::Store(Reg::A0, Reg::Sb, Reg::Sc),
        A::AddI(Reg::Sc, Reg::Fp, 1),
        A::Store(Reg::A1, Reg::Sb, Reg::Sc),
        A::Beq(Reg::R0, Reg::R0, "head".into()),
        A::Label("head".into()),
        A::AddI(Reg::Sc, Reg::Fp, 0),
        A::Load(Reg::T0, Reg::Sb, Reg::Sc),
        A::AddI(Reg::Sc, Reg::Fp, 1),
        A::Load(Reg::T1, Reg::Sb, Reg::Sc),
        A::Sltu(Reg::T2, Reg::T0, Reg::T1),
        A::Beq(Reg::T2, Reg::R0, "exit".into()),
        A::AddI(Reg::T0, Reg::T0, 1),
        A::AddI(Reg::Sc, Reg::Fp, 0),
        A::Store(Reg::T0, Reg::Sb, Reg::Sc),
        A::Beq(Reg::R0, Reg::R0, "head".into()),
        A::Label("exit".into()),
        A::AddI(Reg::Sc, Reg::Fp, 0),
        A::Load(Reg::Rv0, Reg::Sb, Reg::Sc),
    ]
}
fn run(code: &[A], a: u16, b: u16) -> Machine {
    let mut m = Machine::new(64000);
    m.regs[Reg::Sp as usize] = 1000;
    m.regs[Reg::Fp as usize] = 700;
    m.regs[Reg::A0 as usize] = a;
    m.regs[Reg::A1 as usize] = b;
    for r in [Reg::S0, Reg::S1, Reg::S2, Reg::S3] {
        m.regs[r as usize] = 900 + r as u16;
    }
    m.run(&code[..code.len() - 1]);
    m
}
#[test]
fn counter_and_invariant_stay_in_registers_and_zero_trip_restores_home() {
    let mut code = framed(source());
    promote(&mut code, &function(), &HashMap::new(), 0, 2);
    assert!(code
        .iter()
        .any(|i| matches!(i,A::Comment(s)if s=="Cache private loop cells in unused registers")));
    let code = crate::function::frame::finalize(code, 2);
    for (a, b) in [(0, 0), (7, 0), (0, 19), (7, 19), (65530, 65535)] {
        let m = run(&code, a, b);
        assert_eq!(m.regs[Reg::Rv0 as usize], a.max(b));
        assert_eq!(m.regs[Reg::Fp as usize], 700);
        assert_eq!(m.regs[Reg::Sp as usize], 1000);
        for r in [Reg::S0, Reg::S1, Reg::S2, Reg::S3] {
            assert_eq!(m.regs[r as usize], 900 + r as u16);
        }
    }
    let h = code
        .iter()
        .position(|i| matches!(i,A::Label(s)if s=="head"))
        .unwrap();
    let e = code
        .iter()
        .position(|i| matches!(i,A::Label(s)if s=="exit"))
        .unwrap();
    assert!(!code[h..e]
        .iter()
        .any(|i| matches!(i, A::Load(..) | A::Store(..))));
}
#[test]
fn each_early_exit_flushes_modified_slots() {
    let mut body = source();
    let at = body
        .iter()
        .position(|i| matches!(i, A::AddI(Reg::T0, Reg::T0, 1)))
        .unwrap();
    body.splice(at + 1..at + 1, [A::Li(Reg::T2, 5)]);
    let at = body
        .iter()
        .rposition(|i| matches!(i,A::Beq(_,_,s)if s=="head"))
        .unwrap();
    body.insert(at, A::Beq(Reg::T0, Reg::T2, "early".into()));
    body.extend([
        A::Beq(Reg::R0, Reg::R0, "done".into()),
        A::Label("early".into()),
        A::AddI(Reg::Sc, Reg::Fp, 0),
        A::Load(Reg::Rv0, Reg::Sb, Reg::Sc),
        A::Label("done".into()),
    ]);
    let mut code = framed(body);
    promote(&mut code, &function(), &HashMap::new(), 0, 2);
    let code = crate::function::frame::finalize(code, 2);
    for (a, b, expected) in [(0, 0, 0), (0, 19, 5), (7, 19, 19)] {
        assert_eq!(run(&code, a, b).regs[Reg::Rv0 as usize], expected);
    }
}
#[test]
fn bypass_exit_live_scratch_calls_and_memory_asm_disable_unsafe_promotion() {
    let mut bypass = source();
    bypass.insert(0, A::Beq(Reg::A2, Reg::R0, "exit".into()));
    let mut scratch = source();
    let at = scratch
        .iter()
        .position(|i| matches!(i,A::Label(s)if s=="exit"))
        .unwrap();
    scratch.insert(at + 1, A::Move(Reg::Rv1, Reg::Sc));
    let mut call = source();
    let at = call.iter().position(|i| matches!(i, A::Sltu(..))).unwrap();
    call.insert(at, A::Call("opaque".into()));
    for body in [bypass, scratch, call] {
        let mut code = framed(body);
        let before = code.clone();
        promote(&mut code, &function(), &HashMap::new(), 0, 2);
        assert_eq!(code, before);
    }
    let mut f = function();
    let mut b = BasicBlock::new(0);
    b.instructions.push(I::InlineAsm {
        assembly: "STORE %0, R0, %1".into(),
        inputs: vec![],
        outputs: vec![],
        clobbers: vec!["memory".into()],
    });
    f.blocks.push(b);
    let mut code = framed(source());
    let before = code.clone();
    promote(&mut code, &f, &HashMap::new(), 0, 2);
    assert_eq!(code, before);
}
#[test]
fn only_unescaped_nonvolatile_scalar_allocas_are_private() {
    let mut f = function();
    let mut b = BasicBlock::new(0);
    for id in 0..4 {
        b.instructions.push(I::Alloca {
            result: id,
            alloc_type: IrType::I16,
            count: None,
            result_type: IrType::FatPtr(Box::new(IrType::I16)),
        });
    }
    b.instructions.extend([
        I::Load {
            result: 10,
            ptr: Value::Temp(0),
            result_type: IrType::I16,
            volatile: false,
        },
        I::Load {
            result: 11,
            ptr: Value::Temp(1),
            result_type: IrType::I16,
            volatile: true,
        },
        I::Call {
            result: None,
            function: Value::Function("escape".into()),
            args: vec![Value::Temp(2)],
            result_type: IrType::Void,
            stack_args_from: None,
        },
        I::Store {
            value: Value::Temp(3),
            ptr: Value::Temp(3),
            value_type: IrType::I16,
            volatile: false,
        },
    ]);
    f.blocks.push(b);
    assert_eq!(
        private_cells(&f, &HashMap::from([(0, 0), (1, 1), (2, 2), (3, 3)]), 4, 6),
        BTreeSet::from([0, 4, 5])
    );
}
#[test]
fn register_pressure_keeps_original_stack_contract() {
    let mut body = source();
    let header = body
        .iter()
        .position(|i| matches!(i,A::Label(s)if s=="head"))
        .unwrap()
        + 1;
    for (r, index) in REGS.iter().chain(ARGUMENTS.iter()).zip(0..) {
        body.insert(header + index, A::Store(*r, Reg::Gp, Reg::A3));
    }
    let mut code = framed(body);
    let before = code.clone();
    promote(&mut code, &function(), &HashMap::new(), 0, 2);
    assert_eq!(code, before);
}

#[test]
fn registers_used_only_outside_the_loop_can_cache_private_cells() {
    let mut body = source();
    for &r in &REGS {
        body.insert(0, A::Li(r, 321));
    }
    let mut code = framed(body);
    promote(&mut code, &function(), &HashMap::new(), 0, 2);
    assert!(code
        .iter()
        .any(|i| matches!(i,A::Comment(s)if s=="Cache private loop cells in unused registers")));
    let code = crate::function::frame::finalize(code, 2);
    for (start, limit) in [(0, 0), (7, 0), (0, 19)] {
        assert_eq!(
            run(&code, start, limit).regs[Reg::Rv0 as usize],
            start.max(limit)
        );
    }
}

#[test]
fn incoming_values_live_after_any_loop_exit_are_not_borrowed() {
    let mut body = source();
    for &r in &REGS[3..] {
        body.insert(0, A::Li(r, 10));
    }
    // Every otherwise unused register carries a caller-visible result across
    // the loop, even when the header immediately takes the zero-trip exit.
    for &r in &REGS[3..] {
        body.push(A::Add(Reg::Rv0, Reg::Rv0, r));
    }
    // Keep argument registers occupied as well; this case isolates liveness of
    // otherwise unused allocator registers rather than testing argument reuse.
    let header = body
        .iter()
        .position(|i| matches!(i,A::Label(s)if s=="head"))
        .unwrap()
        + 1;
    for reg in ARGUMENTS {
        body.insert(header, A::AddI(Reg::R0, reg, 0));
    }
    let mut code = framed(body);
    let before = code.clone();
    promote(&mut code, &function(), &HashMap::new(), 0, 2);
    assert_eq!(code, before);
    let code = crate::function::frame::finalize(code, 2);
    assert_eq!(run(&code, 0, 19).regs[Reg::Rv0 as usize], 109);
    assert_eq!(run(&code, 7, 0).regs[Reg::Rv0 as usize], 97);
}

#[test]
fn dead_argument_registers_are_available_after_parameter_setup() {
    let mut body = source();
    let header = body
        .iter()
        .position(|i| matches!(i,A::Label(s)if s=="head"))
        .unwrap()
        + 1;
    for reg in REGS {
        body.insert(header, A::Store(reg, Reg::Gp, Reg::R0));
    }
    let mut code = framed(body);
    promote(&mut code, &function(), &HashMap::new(), 0, 2);
    assert!(code
        .iter()
        .any(|i| matches!(i,A::Load(r,Reg::Sb,Reg::Sc)if ARGUMENTS.contains(r))));
    let code = crate::function::frame::finalize(code, 2);
    for (start, limit) in [(0, 0), (7, 0), (0, 19)] {
        assert_eq!(
            run(&code, start, limit).regs[Reg::Rv0 as usize],
            start.max(limit)
        );
    }
}

#[test]
fn argument_values_used_by_later_calls_remain_live_until_redefined() {
    let mut body = source();
    let header = body
        .iter()
        .position(|i| matches!(i,A::Label(s)if s=="head"))
        .unwrap()
        + 1;
    for reg in REGS {
        body.insert(header, A::Store(reg, Reg::Gp, Reg::R0));
    }
    body.push(A::Call("consume_arguments".into()));
    let mut code = framed(body.clone());
    let before = code.clone();
    promote(&mut code, &function(), &HashMap::new(), 0, 2);
    assert_eq!(code, before);

    let call = body.len() - 1;
    for reg in ARGUMENTS {
        body.insert(call, A::Li(reg, 42));
    }
    let mut code = framed(body);
    promote(&mut code, &function(), &HashMap::new(), 0, 2);
    assert!(code
        .iter()
        .any(|i| matches!(i,A::Load(r,Reg::Sb,Reg::Sc)if ARGUMENTS.contains(r))));
}

#[test]
fn newly_borrowed_saved_registers_are_preserved_by_frame_finalization() {
    let mut body = source();
    for reg in [Reg::T3, Reg::T4, Reg::T5, Reg::T6, Reg::T7] {
        body.insert(0, A::Li(reg, 17));
    }
    let mut code = framed(body);
    promote(&mut code, &function(), &HashMap::new(), 0, 2);
    let code = crate::function::frame::finalize(code, 2);
    assert!(code
        .iter()
        .any(|i| matches!(i, A::Store(Reg::S0, Reg::Sb, Reg::Sp))));
    assert!(code
        .iter()
        .any(|i| matches!(i, A::Store(Reg::S1, Reg::Sb, Reg::Sp))));
    let m = run(&code, 0, 19);
    assert_eq!(m.regs[Reg::Rv0 as usize], 19);
    for reg in [Reg::S0, Reg::S1, Reg::S2, Reg::S3] {
        assert_eq!(m.regs[reg as usize], 900 + reg as u16);
    }
}

#[test]
fn nested_loop_initializes_cached_cells_on_each_outer_iteration() {
    let mut inner = source();
    inner.insert(0, A::Label("outer".into()));
    inner.insert(0, A::Li(Reg::X2, 0));
    inner.insert(0, A::Li(Reg::X3, 0));
    inner.extend([
        A::Add(Reg::X3, Reg::X3, Reg::Rv0),
        A::AddI(Reg::X2, Reg::X2, 1),
        A::Li(Reg::Rv1, 3),
        A::Bne(Reg::X2, Reg::Rv1, "outer".into()),
        A::Move(Reg::Rv0, Reg::X3),
    ]);
    let mut code = framed(inner);
    promote(&mut code, &function(), &HashMap::new(), 0, 2);
    assert!(code
        .iter()
        .any(|i| matches!(i,A::Comment(s)if s=="Cache private loop cells in unused registers")));
    let code = crate::function::frame::finalize(code, 2);
    assert_eq!(run(&code, 0, 19).regs[Reg::Rv0 as usize], 57);
    assert_eq!(run(&code, 7, 0).regs[Reg::Rv0 as usize], 21);
}

#[test]
fn discarded_zero_register_write_cannot_be_coalesced_into_a_value() {
    let mut code = vec![A::AddI(Reg::R0, Reg::A0, 5), A::Move(Reg::Rv0, Reg::R0)];
    cleanup(&mut code);
    let mut machine = Machine::new(64000);
    machine.regs[Reg::A0 as usize] = 37;
    machine.run(&code);
    assert_eq!(machine.regs[Reg::Rv0 as usize], 0);
}

#[test]
fn unreachable_comments_after_jumps_do_not_create_loop_entries() {
    let body = source()
        .into_iter()
        .flat_map(|inst| {
            let jump = branch(&inst).is_some();
            let mut out = vec![inst];
            if jump {
                out.push(A::Comment("Branch to next block".into()));
            }
            out
        })
        .collect();
    let mut code = framed(body);
    promote(&mut code, &function(), &HashMap::new(), 0, 2);
    assert!(code
        .iter()
        .any(|i| matches!(i,A::Comment(s)if s=="Cache private loop cells in unused registers")));
    let code = crate::function::frame::finalize(code, 2);
    assert_eq!(run(&code, 0, 19).regs[Reg::Rv0 as usize], 19);
}

#[test]
fn unknown_stack_addresses_disable_promotion_and_r0_is_not_an_address_alias() {
    let mut body = source();
    let at = body.iter().position(|i| matches!(i, A::Sltu(..))).unwrap();
    body.insert(at, A::Load(Reg::Rv1, Reg::Sb, Reg::A2));
    let mut code = framed(body);
    let before = code.clone();
    promote(&mut code, &function(), &HashMap::new(), 0, 2);
    assert_eq!(code, before);

    let code = vec![
        A::Add(Reg::R0, Reg::Fp, Reg::R0),
        A::Load(Reg::T0, Reg::Sb, Reg::R0),
    ];
    assert_eq!(accesses(&code, &Cfg::new(&code).unwrap()), vec![None, None]);
}

#[test]
fn multiple_latches_keep_one_register_value_and_restore_the_home() {
    let mut body = source();
    let at = body
        .iter()
        .rposition(|i| matches!(i, A::Beq(_, _, s) if s=="head"))
        .unwrap();
    body.splice(
        at..at,
        [A::Li(Reg::T2, 5), A::Blt(Reg::T0, Reg::T2, "head".into())],
    );
    let mut code = framed(body);
    promote(&mut code, &function(), &HashMap::new(), 0, 2);
    assert!(code
        .iter()
        .any(|i| matches!(i,A::Comment(s)if s=="Cache private loop cells in unused registers")));
    let code = crate::function::frame::finalize(code, 2);
    assert_eq!(run(&code, 0, 19).regs[Reg::Rv0 as usize], 19);
}
