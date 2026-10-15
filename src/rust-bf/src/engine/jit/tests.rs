use super::*;

fn forced(program: &Program) -> Jit<'_> {
    let mut jit = Jit::new(program).unwrap();
    jit.gate.slots[0] = NativeGate::HOT;
    jit.install(0, Region::collect(program, 0, false).unwrap())
        .unwrap();
    assert_eq!(jit.stats.compiled_regions, 1);
    jit
}

fn same(a: &mut Machine, b: &mut Machine) {
    assert_eq!(a.pointer, b.pointer);
    assert_eq!(a.pc, b.pc);
    assert_eq!(a.pending_input, b.pending_input);
    assert_eq!(a.dispatches, b.dispatches);
    for at in 0..a.tape_len() {
        assert_eq!(a.read_cell(at), b.read_cell(at), "cell {at}");
    }
}

#[test]
fn native_regions_match_every_small_budget_and_wrapping_alias() {
    for (source, optimize) in [
        ("[>+<--]>[-]<++", true),
        ("[->+>+++<<]>[-<+>]>[-<<+>>]<<", true),
        ("[>+<--]>[-]<++", false),
        ("[>+]", false),
        ("[<+>]", false),
        ("[>+[<+>-]<--]", false),
        ("[>+[<+>-]<--]", true),
        (">+<+[>+<--]", true),
    ] {
        for cells in [1, 2, 3, 32] {
            for wrapping in [false, true] {
                let program = Program::parse(source, optimize, false).unwrap();
                let mut jit = forced(&program);
                let mut a = Machine::allocate(cells, wrapping).unwrap();
                let mut b = Machine::allocate(cells, wrapping).unwrap();
                a.write_cell(0, 12);
                b.write_cell(0, 12);
                for turn in 0..100 {
                    let budget = [0, 1, 2, 3, 7, 31][turn % 6];
                    let want = a.run(&program, budget);
                    let got = jit.run(&mut b, budget);
                    assert_eq!(got, want, "{source}, cells={cells}, wrapping={wrapping}");
                    same(&mut a, &mut b);
                    if !matches!(got, Ok(Stop::Budget)) {
                        break;
                    }
                }
            }
        }
    }
}

#[test]
fn moving_native_loops_cross_windows_and_tape_edges_with_exact_partial_state() {
    let program = Program::parse("[>+]", false, false).unwrap();
    for wrapping in [false, true] {
        let mut jit = forced(&program);
        let mut plain = Machine::allocate(4096, wrapping).unwrap();
        let mut native = Machine::allocate(4096, wrapping).unwrap();
        plain.write_cell(0, 1);
        native.write_cell(0, 1);
        for _ in 0..8 {
            let want = plain.run(&program, 4096);
            let got = jit.run(&mut native, 4096);
            assert_eq!(got, want);
            same(&mut plain, &mut native);
            if !matches!(got, Ok(Stop::Budget)) {
                break;
            }
        }
        assert!(jit.stats.native_dispatches > 0);
    }
}

#[test]
fn affine_odd_steps_and_multi_destination_updates_wrap_at_16_bits() {
    for step in [1, 3, 5, 255, 65535] {
        let sign = if step == 65535 {
            "-".into()
        } else {
            "+".repeat(step)
        };
        let source = format!("[{sign}>+++>-----<<]>[<+>-]<");
        let program = Program::parse(&source, true, false).unwrap();
        let mut jit = forced(&program);
        for control in [0, 1, 2, 32767, 32768, 65535] {
            let mut a = Machine::new(8).unwrap();
            let mut b = Machine::new(8).unwrap();
            for machine in [&mut a, &mut b] {
                machine.write_cell(0, control);
                machine.write_cell(1, 65534);
                machine.write_cell(2, 65533);
            }
            assert_eq!(jit.run(&mut b, 100).unwrap(), a.run(&program, 100).unwrap());
            same(&mut a, &mut b);
        }
        assert!(jit.stats.native_dispatches > 0);
    }
}

#[test]
fn intermediate_excursions_trap_or_fall_back_before_any_reordered_write() {
    let program = Program::parse("+>+>>><<<+<+>+", true, false).unwrap();
    let mut jit = Jit::new(&program).unwrap();
    // Install starting at each legal region even if a small summary is rejected.
    for pc in 0..program.code.len() {
        jit.gate.slots[pc] = NativeGate::HOT;
        if let Some(region) = Region::collect(&program, pc, false) {
            jit.install(pc, region).unwrap();
        }
    }
    for cells in [1, 2, 3, 8] {
        let mut a = Machine::new(cells).unwrap();
        let mut b = Machine::new(cells).unwrap();
        assert_eq!(jit.run(&mut b, 100), a.run(&program, 100));
        same(&mut a, &mut b);
    }
}

#[test]
fn input_output_and_hooks_keep_exact_state_and_allow_host_mutations() {
    let program = Program::parse("[>+<--]>+<+?.,$>+<+>+<+!", true, true).unwrap();
    let mut jit = forced(&program);
    let mut a = Machine::new(32).unwrap();
    let mut b = Machine::new(32).unwrap();
    a.write_cell(0, 10);
    b.write_cell(0, 10);
    loop {
        let want = a.run(&program, 100).unwrap();
        assert_eq!(jit.run(&mut b, 100).unwrap(), want);
        same(&mut a, &mut b);
        match want {
            Stop::Halted => break,
            Stop::Input => {
                assert_eq!(jit.run(&mut b, 0).unwrap(), Stop::Input);
                a.provide_input(255).unwrap();
                b.provide_input(255).unwrap();
            }
            Stop::Hook(_) => {
                a.write_cell(1, 1234);
                b.write_cell(1, 1234);
            }
            _ => {}
        }
    }
}

#[test]
fn native_writes_invalidate_cached_scans_without_materializing_other_runs() {
    let program = Program::parse(">>>[-]<<<$[>]", true, true).unwrap();
    let mut jit = forced(&program);
    let mut a = Machine::new(4096).unwrap();
    let mut b = Machine::new(4096).unwrap();
    for machine in [&mut a, &mut b] {
        assert!(machine.tape.assign_uniform(0, 2047, 1, 0, 7));
        let (slot, _) = machine.scan_cache.prepare(1, 0);
        machine.scan_cache.remember(slot, 0, 2047);
    }
    assert_eq!(a.run(&program, 100).unwrap(), Stop::Hook(b'$'));
    assert_eq!(jit.run(&mut b, 100).unwrap(), Stop::Hook(b'$'));
    same(&mut a, &mut b);
    assert!(b.tape.uniform_prefix(1536, 1, 512).is_some());
    assert_eq!(jit.run(&mut b, 100).unwrap(), a.run(&program, 100).unwrap());
    same(&mut a, &mut b);
    assert_eq!(b.pointer(), 3);
}

#[test]
fn native_budget_state_can_be_checkpointed_and_resumed_without_native_code() {
    let program = Program::parse("[>+<--]>+<+", true, false).unwrap();
    let mut jit = forced(&program);
    let mut machine = Machine::new(32).unwrap();
    machine.write_cell(0, 400);
    assert_eq!(jit.run(&mut machine, 17).unwrap(), Stop::Budget);
    let mut bytes = Vec::new();
    machine.write_checkpoint(&program, &mut bytes).unwrap();
    let mut restored = Machine::read_checkpoint(&program, &bytes[..], 32).unwrap();
    assert_eq!(
        jit.run(&mut machine, 10000).unwrap(),
        restored.run(&program, 10000).unwrap()
    );
    same(&mut machine, &mut restored);
}

#[test]
fn randomized_regions_match_literals_at_every_event_and_budget() {
    let mut seed = 0x6d97_135bu32;
    let mut random = || {
        seed ^= seed << 13;
        seed ^= seed >> 17;
        seed ^= seed << 5;
        seed
    };
    for _ in 0..48 {
        let mut source = String::from("[>+<--]");
        for _ in 0..8 {
            source.push_str(
                [
                    ">+<",
                    "[->+++<]",
                    ">[<++>-]<",
                    ">><<",
                    "++--",
                    ".",
                    "$",
                    ",",
                ][random() as usize % 8],
            );
        }
        let program = Program::parse(&source, true, true).unwrap();
        let mut jit = forced(&program);
        let mut a = Machine::new(32).unwrap();
        let mut b = Machine::new(32).unwrap();
        for at in 0..32 {
            let value = if at == 0 {
                (random() as u16) & !1
            } else {
                random() as u16
            };
            a.write_cell(at, value);
            b.write_cell(at, value);
        }
        for _ in 0..200 {
            let budget = u64::from(random() % 128);
            let want = a.run(&program, budget);
            assert_eq!(jit.run(&mut b, budget), want, "{source}");
            same(&mut a, &mut b);
            match want {
                Ok(Stop::Input) => {
                    let value = random() as u8;
                    a.provide_input(value).unwrap();
                    b.provide_input(value).unwrap();
                }
                Ok(Stop::Hook(_)) => {
                    let at = random() as usize % 32;
                    let value = random() as u16;
                    a.write_cell(at, value);
                    b.write_cell(at, value);
                }
                Ok(Stop::Budget | Stop::Output(_)) => {}
                _ => break,
            }
        }
    }
}

#[test]
fn zero_budget_does_not_compile_or_mutate_and_overflow_is_explicit() {
    let program = Program::parse("[>+<--]>+<+", true, false).unwrap();
    let mut jit = Jit::new(&program).unwrap();
    let mut machine = Machine::new(16).unwrap();
    machine.write_cell(0, 8);
    assert_eq!(jit.run(&mut machine, 0).unwrap(), Stop::Budget);
    assert!(jit.gate.slots.iter().all(|&heat| heat == 0));
    assert_eq!(machine.dispatches, 0);
    jit = forced(&program);
    machine.dispatches = u64::MAX;
    assert_eq!(jit.run(&mut machine, 1), Err(Error::DispatchOverflow));
    assert_eq!(machine.read_cell(0), 8);
}

#[test]
fn short_regions_retire_once_and_acyclic_regions_never_compile() {
    let program = Program::parse("[>+<--]", false, false).unwrap();
    let mut jit = Jit::new(&program).unwrap();
    let mut machine = Machine::new(32).unwrap();
    machine.write_cell(0, 4000);
    while jit.run(&mut machine, 4).unwrap() != Stop::Halted {}
    assert!(jit.stats.native_dispatches > 0);
    assert_eq!(jit.stats.compiled_regions, 1);
    assert_eq!(jit.stats.retired_regions, 1);
    assert_eq!(machine.read_cell(1), 2000);
    let program = Program::parse("+>+<", false, false).unwrap();
    let mut jit = Jit::new(&program).unwrap();
    jit.warm(0).unwrap();
    assert_eq!(jit.stats.compiled_regions, 0);
    // A backward branch is insufficient if a Rust-only scan breaks the cycle.
    let program = Program::parse("[>+<--]", false, false).unwrap();
    let mut region = Region::collect(&program, 0, false).unwrap();
    assert!(region.has_cycle());
    region.nodes.remove(&2); // An operation delegated to Rust breaks the cycle.
    assert!(!region.has_cycle());
}

#[test]
fn native_windows_preserve_all_logical_tape_values_and_outside_progressions() {
    for step in [1, 2, 3, 8, 37, 65] {
        for low in [0, 1, 63, 1024, 2048, 2976] {
            let mut tape = super::super::tape::Tape::new(vec![0; 8192]);
            let high = (low + 1024).min(4096);
            let last = 16 + (8191 - 16) / step * step;
            assert!(tape.assign_uniform(16, last, step, 0, 7));
            let before: Vec<_> = (0..8192).map(|i| tape[i]).collect();
            let ptr = tape.prepare_native(low, high);
            assert!(before.iter().enumerate().all(|(i, &v)| tape[i] == v));
            // Every native-write position must have lost its deferred alias.
            for i in low..high {
                unsafe {
                    *ptr.add(i) = 9;
                }
                assert_eq!(tape[i], 9);
            }
            for i in 0..8192 {
                assert_eq!(
                    tape[i],
                    if (low..high).contains(&i) {
                        9
                    } else {
                        before[i]
                    }
                );
            }
        }
    }
}

#[test]
fn range_invalidation_retains_only_true_facts_across_every_cached_residue() {
    for stride in [1, 2, 3, 8, 37, 65] {
        for low in [0, 1, 17, 64, 127, 1024] {
            let mut cache = super::super::scan::Cache::default();
            for residue in 0..stride.min(32) {
                let (slot, _) = cache.prepare(stride, residue);
                cache.remember(slot, residue, residue + stride * 100);
            }
            let high = low + 63;
            cache.invalidate_range(low, high);
            for index in 0..stride * 100 {
                if cache.known_run(stride, index).is_some() {
                    assert!(
                        !(low..=high).contains(&index),
                        "stride={stride}, range={low}..{high}, index={index}"
                    );
                }
            }
        }
    }
}
