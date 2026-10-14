use super::*;
use crate::instr::tests::machine::Machine;
use rcc_ir::ir::{BasicBlock, IrType};
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

fn compare_cleanup(source: &[A], inputs: &[(Reg, u16)]) -> Vec<A> {
    let mut actual = source.to_vec();
    cleanup(&mut actual);
    let execute = |code: &[A]| {
        let mut machine = Machine::new(64000);
        for &(reg, value) in inputs {
            machine.regs[reg as usize] = value;
        }
        machine.run(code);
        (
            machine.regs[Reg::Rv0 as usize],
            machine.regs[Reg::Rv1 as usize],
        )
    };
    assert_eq!(execute(&actual), execute(source));
    actual
}

#[test]
fn copies_cross_branches_and_disappear_when_all_reads_use_the_original() {
    let code = vec![
        A::Move(Reg::T0, Reg::S0),
        A::Beq(Reg::A0, Reg::R0, "exit".into()),
        A::AddI(Reg::Rv0, Reg::T0, 7),
        A::Beq(Reg::R0, Reg::R0, "done".into()),
        A::Label("exit".into()),
        A::AddI(Reg::Rv0, Reg::T0, 3),
        A::Label("done".into()),
    ];
    for flag in [0, 1] {
        let actual = compare_cleanup(&code, &[(Reg::S0, 65534), (Reg::A0, flag)]);
        assert!(!actual.iter().any(|i| matches!(i, A::Move(Reg::T0, _))));
    }
}

#[test]
fn unequal_join_values_and_conditional_clobbers_keep_their_path_values() {
    for second in [A::Move(Reg::T0, Reg::A1), A::AddI(Reg::A0, Reg::A0, 1)] {
        let code = vec![
            A::Move(Reg::T0, Reg::A0),
            A::Beq(Reg::A2, Reg::R0, "join".into()),
            second,
            A::Label("join".into()),
            A::Move(Reg::Rv0, Reg::T0),
            A::Move(Reg::Rv1, Reg::A0),
        ];
        for flag in [0, 1] {
            compare_cleanup(&code, &[(Reg::A0, 17), (Reg::A1, 42), (Reg::A2, flag)]);
        }
    }
}

#[test]
fn entry_backedges_never_invent_first_iteration_equalities() {
    let code = vec![
        A::Label("head".into()),
        A::Add(Reg::Rv0, Reg::Rv0, Reg::T0),
        A::Beq(Reg::A0, Reg::R0, "exit".into()),
        A::Move(Reg::T0, Reg::S0),
        A::AddI(Reg::A0, Reg::A0, -1),
        A::Beq(Reg::R0, Reg::R0, "head".into()),
        A::Label("exit".into()),
    ];
    for count in [0, 1, 5] {
        compare_cleanup(&code, &[(Reg::A0, count), (Reg::T0, 7), (Reg::S0, 19)]);
    }
}

#[test]
fn copy_facts_survive_only_matching_reachable_predecessors() {
    let code = vec![
        A::Move(Reg::T0, Reg::S0),
        A::Beq(Reg::R0, Reg::R0, "join".into()),
        A::Label("unreachable".into()),
        A::Move(Reg::T0, Reg::A1),
        A::Beq(Reg::R0, Reg::R0, "join".into()),
        A::Label("join".into()),
        A::Move(Reg::Rv0, Reg::T0),
    ];
    let actual = compare_cleanup(&code, &[(Reg::S0, 99), (Reg::A1, 4)]);
    assert!(actual
        .iter()
        .any(|i| matches!(i, A::Move(Reg::Rv0, Reg::S0))));
}

#[test]
fn calls_and_unknown_effects_kill_all_copy_facts() {
    for barrier in [A::Call("callee".into()), A::Raw("opaque".into())] {
        let mut code = vec![
            A::Move(Reg::T0, Reg::S0),
            barrier,
            A::Move(Reg::Rv0, Reg::T0),
        ];
        let cfg = Cfg::new(&code).unwrap();
        propagate_copies(&mut code, &cfg);
        assert!(matches!(code[2], A::Move(Reg::Rv0, Reg::T0)));
    }
}

#[test]
fn redefinitions_preserve_copied_old_values_and_immutable_zero() {
    let code = vec![
        A::Move(Reg::T0, Reg::S0),
        A::Move(Reg::T1, Reg::T0),
        A::Li(Reg::S0, 55),
        A::Move(Reg::Rv0, Reg::T1),
        A::Li(Reg::T2, 0),
        A::AddI(Reg::R0, Reg::S0, 9),
        A::Move(Reg::Rv1, Reg::T2),
    ];
    compare_cleanup(&code, &[(Reg::S0, 29)]);
    let mut equal = identity_equalities();
    transfer_equalities(&A::Move(Reg::T0, Reg::S0), &mut equal);
    transfer_equalities(&A::Move(Reg::T0, Reg::T0), &mut equal);
    assert_ne!(equal[Reg::T0 as usize] & bit(Reg::S0), 0);
    transfer_equalities(&A::Load(Reg::S0, Reg::Gp, Reg::A0), &mut equal);
    assert_eq!(equal[Reg::T0 as usize], bit(Reg::T0));
}

#[test]
fn second_round_reuses_freed_registers_without_disturbing_first_cache_or_exits() {
    let mut body = source();
    let at = body
        .iter()
        .position(|i| matches!(i, A::Label(s) if s == "head"))
        .unwrap()
        + 1;
    let occupied = [
        Reg::T3,
        Reg::T4,
        Reg::T5,
        Reg::T6,
        Reg::T7,
        Reg::A0,
        Reg::A1,
        Reg::A2,
    ];
    body.splice(
        at..at,
        occupied.into_iter().map(|r| A::Store(r, Reg::Gp, Reg::R0)),
    );
    // Before cleanup only A3 is free, so the first round caches the counter.
    // Its cleanup removes these dead writes, making saved registers available
    // to the second round for the remaining loop-invariant bound.
    body.splice(
        at..at,
        [Reg::S0, Reg::S1, Reg::S2, Reg::S3]
            .into_iter()
            .map(|r| A::Li(r, 23)),
    );
    let at = body
        .iter()
        .rposition(|i| matches!(i, A::Beq(_, _, s) if s == "head"))
        .unwrap();
    body.splice(
        at..at,
        [A::Li(Reg::T2, 5), A::Beq(Reg::T0, Reg::T2, "early".into())],
    );
    body.extend([
        A::Beq(Reg::R0, Reg::R0, "done".into()),
        A::Label("early".into()),
        A::AddI(Reg::Sc, Reg::Fp, 0),
        A::Load(Reg::Rv0, Reg::Sb, Reg::Sc),
        A::Label("done".into()),
    ]);
    let mut code = framed(body);
    promote(&mut code, &function(), &HashMap::new(), 0, 2);
    assert_eq!(code.iter().filter(|i| matches!(i, A::Comment(s) if s == "Cache private loop cells in unused registers")).count(), 2);
    let header = code
        .iter()
        .position(|i| matches!(i, A::Label(s) if s == "head"))
        .unwrap();
    let exit = code
        .iter()
        .position(|i| matches!(i, A::Label(s) if s == "exit"))
        .unwrap();
    assert!(!code[header..exit]
        .iter()
        .any(|i| matches!(i, A::Load(_, Reg::Sb, _) | A::Store(_, Reg::Sb, _))));
    let code = crate::function::frame::finalize(code, 2);
    assert!(code
        .iter()
        .any(|i| matches!(i, A::Store(Reg::S0, Reg::Sb, Reg::Sp))));
    for (start, limit, expected) in [(0, 0, 0), (7, 0, 7), (0, 19, 5), (7, 19, 19)] {
        let m = run(&code, start, limit);
        assert_eq!(m.regs[Reg::Rv0 as usize], expected);
        assert_eq!(m.regs[Reg::Sp as usize], 1000);
        assert_eq!(m.regs[Reg::Fp as usize], 700);
        for r in [Reg::S0, Reg::S1, Reg::S2, Reg::S3] {
            assert_eq!(m.regs[r as usize], 900 + r as u16);
        }
    }
}

fn compare_private_cleanup(code: &[A], inputs: &[(Reg, u16)], private: &[i16]) -> Vec<A> {
    let mut optimized = code.to_vec();
    cleanup_private(&mut optimized, &private.iter().copied().collect());
    let execute = |instructions: &[A]| {
        let mut m = Machine::new(64000);
        m.regs[Reg::Fp as usize] = 1000;
        for &(reg, value) in inputs {
            m.regs[reg as usize] = value;
        }
        m.run(instructions);
        (
            m.regs[Reg::Rv0 as usize],
            m.regs[Reg::Rv1 as usize],
            m.memory
                .into_iter()
                .filter(|((bank, address), _)| {
                    *bank != 2 || !private.contains(&((*address as i16).wrapping_sub(1000)))
                })
                .collect::<BTreeMap<_, _>>(),
        )
    };
    assert_eq!(execute(&optimized), execute(code));
    optimized
}

#[test]
fn private_preloads_definitely_overwritten_and_dead_flushes_disappear() {
    let code = vec![
        A::AddI(Reg::Sc, Reg::Fp, 0),
        A::Store(Reg::A0, Reg::Sb, Reg::Sc),
        A::Load(Reg::T0, Reg::Sb, Reg::Sc),
        A::Li(Reg::T0, 123),
        A::Beq(Reg::A1, Reg::R0, "exit".into()),
        A::AddI(Reg::T0, Reg::T0, 7),
        A::Label("exit".into()),
        A::AddI(Reg::Sc, Reg::Fp, 0),
        A::Store(Reg::T0, Reg::Sb, Reg::Sc),
        A::Move(Reg::Rv0, Reg::T0),
    ];
    for flag in [0, 1] {
        let optimized = compare_private_cleanup(&code, &[(Reg::A0, 51), (Reg::A1, flag)], &[0]);
        assert!(!optimized
            .iter()
            .any(|i| matches!(i, A::Load(..) | A::Store(..))));
    }
}

#[test]
fn private_store_needed_on_one_path_survives_and_overwritten_store_dies() {
    let code = vec![
        A::AddI(Reg::Sc, Reg::Fp, 0),
        A::Store(Reg::A0, Reg::Sb, Reg::Sc),
        A::Beq(Reg::A2, Reg::R0, "join".into()),
        A::Store(Reg::A1, Reg::Sb, Reg::Sc),
        A::Label("join".into()),
        A::AddI(Reg::Sc, Reg::Fp, 0),
        A::Load(Reg::Rv0, Reg::Sb, Reg::Sc),
        A::Store(Reg::A0, Reg::Sb, Reg::Sc),
        A::Store(Reg::A1, Reg::Sb, Reg::Sc),
        A::Load(Reg::Rv1, Reg::Sb, Reg::Sc),
    ];
    for flag in [0, 1] {
        let optimized = compare_private_cleanup(
            &code,
            &[(Reg::A0, 12), (Reg::A1, 56), (Reg::A2, flag)],
            &[0],
        );
        assert_eq!(
            optimized
                .iter()
                .filter(|i| matches!(i, A::Store(..)))
                .count(),
            3
        );
    }
}

#[test]
fn private_home_liveness_crosses_backedges_and_keeps_zero_trip_value() {
    let code = vec![
        A::AddI(Reg::Sc, Reg::Fp, 0),
        A::Store(Reg::A0, Reg::Sb, Reg::Sc),
        A::Label("head".into()),
        A::Beq(Reg::A1, Reg::R0, "exit".into()),
        A::AddI(Reg::Sc, Reg::Fp, 0),
        A::Load(Reg::T0, Reg::Sb, Reg::Sc),
        A::AddI(Reg::T0, Reg::T0, 9),
        A::Store(Reg::T0, Reg::Sb, Reg::Sc),
        A::AddI(Reg::A1, Reg::A1, -1),
        A::Beq(Reg::R0, Reg::R0, "head".into()),
        A::Label("exit".into()),
        A::AddI(Reg::Sc, Reg::Fp, 0),
        A::Load(Reg::Rv0, Reg::Sb, Reg::Sc),
    ];
    for count in [0, 1, 19] {
        let optimized = compare_private_cleanup(&code, &[(Reg::A0, 33), (Reg::A1, count)], &[0]);
        assert_eq!(
            optimized
                .iter()
                .filter(|i| matches!(i, A::Store(..)))
                .count(),
            2
        );
    }
}

#[test]
fn opaque_reads_calls_and_nonprivate_homes_keep_memory_effects() {
    for barrier in [
        A::Call("callee".into()),
        A::Raw("opaque".into()),
        A::Load(Reg::Rv0, Reg::A0, Reg::A1),
        A::Load(Reg::Rv0, Reg::Sb, Reg::A1),
    ] {
        let mut code = vec![
            A::AddI(Reg::Sc, Reg::Fp, 0),
            A::Store(Reg::A2, Reg::Sb, Reg::Sc),
            barrier,
        ];
        discard_dead_private_accesses(&mut code, &BTreeSet::from([0]));
        assert!(matches!(code[1], A::Store(..)));
    }
    let code = vec![
        A::AddI(Reg::Sc, Reg::Fp, 1),
        A::Load(Reg::T0, Reg::Sb, Reg::Sc),
        A::Store(Reg::A0, Reg::Sb, Reg::Sc),
    ];
    let mut optimized = code.clone();
    discard_dead_private_accesses(&mut optimized, &BTreeSet::from([0]));
    assert_eq!(
        optimized, code,
        "escaped and volatile homes are not private"
    );
}

fn comparison_source(equal: bool, on_zero: bool, reused: bool) -> Vec<A> {
    let mut code = vec![
        A::Xor(Reg::T0, Reg::A0, Reg::A1),
        A::Li(Reg::T1, if equal { 1 } else { 0 }),
        if equal {
            A::Sltu(Reg::T2, Reg::T0, Reg::T1)
        } else {
            A::Sltu(Reg::T2, Reg::T1, Reg::T0)
        },
        if on_zero {
            A::Beq(Reg::T2, Reg::R0, "taken".into())
        } else {
            A::Bne(Reg::T2, Reg::R0, "taken".into())
        },
        A::Li(Reg::Rv0, 17),
        A::Beq(Reg::R0, Reg::R0, "exit".into()),
        A::Label("taken".into()),
        A::Li(Reg::Rv0, 99),
        A::Label("exit".into()),
    ];
    if reused {
        code.push(A::Move(Reg::Rv1, Reg::T2));
    }
    code
}

#[test]
fn equality_and_inequality_branches_avoid_boolean_materialization() {
    for equal in [false, true] {
        for on_zero in [false, true] {
            let code = comparison_source(equal, on_zero, false);
            for (a, b) in [(0, 0), (7, 7), (0, 65535), (32768, 32767)] {
                let optimized = compare_cleanup(&code, &[(Reg::A0, a), (Reg::A1, b)]);
                assert!(!optimized
                    .iter()
                    .any(|i| matches!(i, A::Xor(..) | A::Sltu(..))));
                assert!(optimized.iter().any(|i| matches!(
                    i,
                    A::Beq(Reg::A0, Reg::A1, _) | A::Bne(Reg::A0, Reg::A1, _)
                )));
            }
        }
    }
}

#[test]
fn direct_comparison_branches_do_not_require_a_promoted_loop() {
    let source = framed(comparison_source(false, true, false));
    let mut optimized = source.clone();
    promote(&mut optimized, &function(), &HashMap::new(), 0, 0);
    assert!(!optimized
        .iter()
        .any(|i| matches!(i, A::Xor(..) | A::Sltu(..))));
    for (a, b) in [(5, 5), (5, 19)] {
        assert_eq!(
            run(&optimized, a, b).regs[Reg::Rv0 as usize],
            run(&source, a, b).regs[Reg::Rv0 as usize]
        );
    }
}

#[test]
fn equality_predicates_reused_after_branch_keep_their_value() {
    for equal in [false, true] {
        let code = comparison_source(equal, true, true);
        for (a, b) in [(4, 4), (5, 7), (0, 65535)] {
            let optimized = compare_cleanup(&code, &[(Reg::A0, a), (Reg::A1, b)]);
            assert!(optimized.iter().any(|i| matches!(i, A::Sltu(..))));
        }
    }
}

#[test]
fn equality_branch_fusion_rejects_changed_operands_and_opaque_barriers() {
    for barrier in [
        A::Li(Reg::A0, 123),
        A::Call("callee".into()),
        A::Raw("opaque".into()),
    ] {
        let mut code = comparison_source(false, true, false);
        code.insert(3, barrier.clone());
        if matches!(barrier, A::Li(..)) {
            compare_cleanup(&code, &[(Reg::A0, 7), (Reg::A1, 7)]);
        }
        let cfg = Cfg::new(&code).unwrap();
        fuse_comparison_branches(&mut code, &cfg);
        assert!(matches!(code[4], A::Beq(Reg::T2, Reg::R0, _)));
    }
    let mut code = comparison_source(false, true, false);
    code[0] = A::Xor(Reg::A0, Reg::A0, Reg::A1);
    code[2] = A::Sltu(Reg::T2, Reg::T1, Reg::A0);
    for (a, b) in [(7, 7), (123, 7)] {
        compare_cleanup(&code, &[(Reg::A0, a), (Reg::A1, b)]);
    }
}

#[test]
fn equality_fusion_uses_preserved_copy_when_boolean_reuses_operand_register() {
    for is_equal in [false, true] {
        for reused in [false, true] {
            let mut code = vec![
                A::AddI(Reg::T2, Reg::A0, 1),
                A::Move(Reg::S0, Reg::T2),
                A::Xor(Reg::T7, Reg::T2, Reg::A1),
                A::Li(Reg::T1, if is_equal { 1 } else { 0 }),
                if is_equal {
                    A::Sltu(Reg::T2, Reg::T7, Reg::T1)
                } else {
                    A::Sltu(Reg::T2, Reg::T1, Reg::T7)
                },
                A::Beq(Reg::T2, Reg::R0, "taken".into()),
                A::AddI(Reg::Rv0, Reg::S0, 17),
                A::Beq(Reg::R0, Reg::R0, "done".into()),
                A::Label("taken".into()),
                A::AddI(Reg::Rv0, Reg::S0, 99),
                A::Label("done".into()),
            ];
            if reused {
                code.push(A::Move(Reg::Rv1, Reg::T2));
            }
            let mut fused = code.clone();
            let cfg = Cfg::new(&fused).unwrap();
            fuse_comparison_branches(&mut fused, &cfg);
            assert!(matches!(
                fused[5],
                A::Beq(Reg::S0, Reg::A1, _) | A::Bne(Reg::S0, Reg::A1, _)
            ));
            for (a, b) in [(7, 8), (7, 19), (65535, 0)] {
                let optimized = compare_cleanup(&code, &[(Reg::A0, a), (Reg::A1, b)]);
                assert!(
                    optimized.iter().any(|inst| matches!(
                        inst,
                        A::Beq(Reg::S0, Reg::A1, _) | A::Bne(Reg::S0, Reg::A1, _)
                    )),
                    "equal={is_equal} reused={reused}: {optimized:#?}"
                );
                assert_eq!(optimized.iter().any(|i| matches!(i, A::Sltu(..))), reused);
                if !reused {
                    assert!(!optimized.iter().any(|i| matches!(i, A::Xor(..))));
                }
            }
        }
    }
}

#[test]
fn comparison_alias_must_survive_until_the_branch() {
    for replacement in [
        A::Li(Reg::S0, 55),
        A::Call("callee".into()),
        A::Raw("opaque".into()),
    ] {
        let mut code = vec![
            A::Move(Reg::S0, Reg::A0),
            A::Xor(Reg::T7, Reg::A0, Reg::A1),
            A::Sltu(Reg::A0, Reg::R0, Reg::T7),
            replacement.clone(),
            A::Beq(Reg::A0, Reg::R0, "done".into()),
            A::Li(Reg::Rv0, 19),
            A::Label("done".into()),
        ];
        if matches!(replacement, A::Li(..)) {
            for (a, b) in [(7, 7), (3, 55)] {
                compare_cleanup(&code, &[(Reg::A0, a), (Reg::A1, b)]);
            }
        }
        let cfg = Cfg::new(&code).unwrap();
        fuse_comparison_branches(&mut code, &cfg);
        assert!(matches!(code[4], A::Beq(Reg::A0, Reg::R0, _)));
    }
}

#[test]
fn cached_argument_and_temporary_sources_do_not_need_per_iteration_copies() {
    let code = vec![
        A::Li(Reg::T1, 100),
        A::Li(Reg::S0, 0),
        A::Label("head".into()),
        A::Move(Reg::T2, Reg::A1),
        A::Sltu(Reg::T4, Reg::S0, Reg::T2),
        A::Beq(Reg::T4, Reg::R0, "exit".into()),
        A::Move(Reg::T0, Reg::A0),
        A::Add(Reg::S2, Reg::T0, Reg::S0),
        A::Move(Reg::T5, Reg::T1),
        A::Add(Reg::T3, Reg::T5, Reg::S2),
        A::Move(Reg::T4, Reg::A2),
        A::Store(Reg::T4, Reg::R0, Reg::T3),
        A::AddI(Reg::T2, Reg::S0, 1),
        A::Move(Reg::S0, Reg::T2),
        A::Beq(Reg::R0, Reg::R0, "head".into()),
        A::Label("exit".into()),
        A::Move(Reg::Rv0, Reg::S0),
    ];
    for count in [0, 1, 19] {
        let optimized =
            compare_private_cleanup(&code, &[(Reg::A0, 5), (Reg::A1, count), (Reg::A2, 42)], &[]);
        let start = optimized
            .iter()
            .position(|i| matches!(i, A::Label(s) if s == "head"))
            .unwrap();
        let end = optimized
            .iter()
            .position(|i| matches!(i, A::Label(s) if s == "exit"))
            .unwrap();
        assert!(
            !optimized[start..end]
                .iter()
                .any(|i| matches!(i, A::Move(..))),
            "{optimized:#?}"
        );
    }
}

#[test]
fn zero_bitwise_copies_disappear_without_a_promoted_loop() {
    let source = framed(vec![
        A::Xor(Reg::T0, Reg::A0, Reg::R0),
        A::Or(Reg::T1, Reg::R0, Reg::T0),
        A::Move(Reg::Rv0, Reg::T1),
    ]);
    let mut optimized = source.clone();
    promote(&mut optimized, &function(), &HashMap::new(), 0, 0);
    assert!(!optimized.iter().any(|i| matches!(i, A::Xor(..) | A::Or(..))));
    for value in [0, 1, 32768, 65535] {
        assert_eq!(run(&optimized, value, 0).regs[Reg::Rv0 as usize], value);
    }
}

#[test]
fn zero_bitwise_copies_keep_old_sources_at_unequal_joins() {
    let code = vec![
        A::Xor(Reg::T0, Reg::R0, Reg::A0),
        A::Beq(Reg::A2, Reg::R0, "join".into()),
        A::Or(Reg::T0, Reg::A1, Reg::R0),
        A::Label("join".into()),
        A::Or(Reg::Rv0, Reg::R0, Reg::T0),
        A::Xor(Reg::A0, Reg::A0, Reg::R0),
        A::Or(Reg::R0, Reg::A1, Reg::R0),
        A::Xor(Reg::Rv1, Reg::A0, Reg::R0),
    ];
    for flag in [0, 1] {
        let optimized = compare_cleanup(&code, &[(Reg::A0, 65535), (Reg::A1, 1), (Reg::A2, flag)]);
        assert!(!optimized.iter().any(|i| matches!(i, A::Xor(..) | A::Or(..))));
    }
}

#[test]
fn zero_bitwise_copies_use_only_current_proven_zero_operands() {
    let code = vec![
        A::Li(Reg::T0, 0),
        A::Move(Reg::T1, Reg::T0),
        A::Xor(Reg::T2, Reg::A0, Reg::T1),
        A::Li(Reg::T1, 1),
        A::Or(Reg::Rv0, Reg::T2, Reg::T1),
        A::Or(Reg::Rv1, Reg::A0, Reg::A1),
    ];
    for (a, b) in [(0, 0), (65535, 1), (32768, 32768)] {
        let optimized = compare_cleanup(&code, &[(Reg::A0, a), (Reg::A1, b)]);
        assert!(!optimized.iter().any(|i| matches!(i, A::Xor(..))));
        assert_eq!(optimized.iter().filter(|i| matches!(i, A::Or(..))).count(), 2);
    }
    for barrier in [A::Call("callee".into()), A::Raw("opaque".into())] {
        let mut code = vec![A::Li(Reg::T0, 0), barrier, A::Or(Reg::Rv0, Reg::A0, Reg::T0)];
        let cfg = Cfg::new(&code).unwrap();
        assert!(!fold_zero_bitwise_copies(&mut code, &cfg));
        assert!(matches!(code[2], A::Or(..)));
    }
}
