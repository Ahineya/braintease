use super::*;

// Intentionally independent: this oracle retains every BF character, builds its
// own bracket table and executes one literal command at a time. It never calls
// Program's parser, arithmetic summarizer or optimized executor.
struct Literal {
    code: Vec<u8>,
    jumps: Vec<usize>,
    tape: Vec<u16>,
    pointer: usize,
    pc: usize,
    wrapping: bool,
    pending_input: bool,
}

impl Literal {
    fn new(source: &str, cells: usize, wrapping: bool, hooks: bool) -> Self {
        let code: Vec<_> = source
            .bytes()
            .filter(|c| b"+-<>[],.".contains(c) || (hooks && b"?!$".contains(c)))
            .collect();
        let mut jumps = vec![0; code.len()];
        let mut stack = Vec::new();
        for (at, byte) in code.iter().enumerate() {
            if *byte == b'[' {
                stack.push(at);
            }
            if *byte == b']' {
                let start = stack.pop().unwrap();
                jumps[start] = at;
                jumps[at] = start;
            }
        }
        assert!(stack.is_empty());
        Self {
            code,
            jumps,
            tape: vec![0; cells],
            pointer: 0,
            pc: 0,
            wrapping,
            pending_input: false,
        }
    }

    fn event(&mut self) -> Result<Stop, ()> {
        if self.pending_input {
            return Ok(Stop::Input);
        }
        for _ in 0..4_000_000 {
            if self.pc == self.code.len() {
                return Ok(Stop::Halted);
            }
            let byte = self.code[self.pc];
            match byte {
                b'+' => self.tape[self.pointer] = self.tape[self.pointer].wrapping_add(1),
                b'-' => self.tape[self.pointer] = self.tape[self.pointer].wrapping_sub(1),
                b'>' => {
                    if self.pointer + 1 == self.tape.len() {
                        if !self.wrapping {
                            return Err(());
                        }
                        self.pointer = 0;
                    } else {
                        self.pointer += 1;
                    }
                }
                b'<' => {
                    if self.pointer == 0 {
                        if !self.wrapping {
                            return Err(());
                        }
                        self.pointer = self.tape.len() - 1;
                    } else {
                        self.pointer -= 1;
                    }
                }
                b'[' if self.tape[self.pointer] == 0 => self.pc = self.jumps[self.pc],
                b']' if self.tape[self.pointer] != 0 => self.pc = self.jumps[self.pc],
                b',' => {
                    self.pc += 1;
                    self.pending_input = true;
                    return Ok(Stop::Input);
                }
                b'.' => {
                    self.pc += 1;
                    return Ok(Stop::Output(self.tape[self.pointer] as u8));
                }
                b'?' | b'!' | b'$' => {
                    self.pc += 1;
                    return Ok(Stop::Hook(byte));
                }
                _ => {}
            }
            self.pc += 1;
        }
        panic!("literal fixture exceeded its instruction bound");
    }
}

fn event(machine: &mut Machine, program: &Program) -> Result<Stop, Error> {
    for _ in 0..100_000 {
        let result = machine.run(program, 7)?;
        if result != Stop::Budget {
            return Ok(result);
        }
    }
    panic!("optimized fixture exceeded its dispatch bound");
}

pub(super) fn compare(source: &str, initial: &[u16], wrapping: bool, hooks: bool) {
    for optimize in [false, true] {
        let program = Program::parse(source, optimize, hooks).unwrap();
        let mut actual = if wrapping {
            Machine::with_wrapping_tape(initial.len())
        } else {
            Machine::new(initial.len())
        }
        .unwrap();
        let mut expected = Literal::new(source, initial.len(), wrapping, hooks);
        actual.tape_mut().copy_from_slice(initial);
        expected.tape.copy_from_slice(initial);
        let mut inputs = [0x80u8, 0xff, 0, 17].into_iter().cycle();
        let mut ended = false;
        for _ in 0..1000 {
            let got = event(&mut actual, &program);
            let want = expected.event();
            assert_eq!(
                actual.tape(),
                expected.tape,
                "tape differs: {source:?}, optimize={optimize}, wrapping={wrapping}"
            );
            assert_eq!(
                actual.pointer(),
                expected.pointer,
                "pointer differs: {source:?}"
            );
            match (got, want) {
                (Err(Error::TapeBounds { .. }), Err(())) => {
                    ended = true;
                    break;
                }
                (Ok(got), Ok(want)) => {
                    assert_eq!(got, want, "event differs: {source:?}");
                    match got {
                        Stop::Halted => {
                            ended = true;
                            break;
                        }
                        Stop::Input => {
                            let value = inputs.next().unwrap();
                            actual.provide_input(value).unwrap();
                            expected.tape[expected.pointer] = value.into();
                            expected.pending_input = false;
                        }
                        Stop::Hook(_) => {
                            // Native hosts may modify the observed cell and
                            // arbitrary descriptor/response cells synchronously.
                            let pointer = actual.pointer();
                            actual.tape_mut()[pointer] ^= 0x1234;
                            expected.tape[expected.pointer] ^= 0x1234;
                            actual.tape_mut()[0] = actual.tape()[0].wrapping_add(3);
                            expected.tape[0] = expected.tape[0].wrapping_add(3);
                        }
                        _ => {}
                    }
                }
                (got, want) => panic!("different results for {source:?}: {got:?} / {want:?}"),
            }
        }
        assert!(
            ended,
            "fixture did not halt or reach its expected bounds error"
        );
    }
}

#[test]
fn rejects_invalid_configuration_and_brackets() {
    assert!(matches!(Machine::new(0), Err(Error::EmptyTape)));
    assert!(matches!(
        Machine::new(usize::MAX),
        Err(Error::TapeAllocation { .. })
    ));
    for source in ["[", "]", "[]]", "[[+]"] {
        assert!(matches!(
            Program::parse(source, true, false),
            Err(Error::Parse { .. })
        ));
    }
    let empty = Program::parse("comments", true, true).unwrap();
    assert_eq!(
        Machine::new(1).unwrap().run(&empty, 0).unwrap(),
        Stop::Halted
    );
}

#[test]
fn affine_multi_destination_odd_deltas_and_cell_wrapping() {
    for value in [0, 1, 2, 127, 32768, 65535] {
        for source in [
            "[->+>+++<<]",
            "[>+>---<<-]",
            "[+>++<]",
            "[--->++<]",
            "[--->++>---<<]",
            "[-]",
            "[+]",
        ] {
            compare(source, &[value, 65535, 3, 9], false, false);
        }
    }
    compare(
        &format!("{}.{}.", "+".repeat(65536), "-".repeat(65537)),
        &[0],
        false,
        false,
    );
}

#[test]
fn strict_bounds_preserve_partial_state_and_zero_loop_skips() {
    for source in [
        "<+", ">>>+", ">+>+", "<>+<.", "+[->+<]", "+[<+>-]", "+[>]", "[<+>-]", "[>>>]", ">+<+>+>>",
    ] {
        compare(source, &[0, 0], false, false);
    }
    compare("+[->>+<<]", &[0, 7], false, false);
    compare("[->>+<<]", &[0, 7], false, false);
}

#[test]
fn wrapping_tape_fallback_preserves_aliases_and_pointer_excursions() {
    for source in [
        "<>+<.",
        ">>>+<<<.",
        "+[->+>+<<]",
        "+[->+<]",
        ">+<[>>]",
        "+[>><]",
    ] {
        compare(source, &[0, 0, 0], true, false);
    }
    // A transfer into its own source must remain nonterminating. The ordinary
    // four-operation strength reduction incorrectly clears this one-cell loop.
    for optimize in [false, true] {
        let program = Program::parse("+[->+<]", optimize, false).unwrap();
        let mut machine = Machine::with_wrapping_tape(1).unwrap();
        assert_eq!(machine.run(&program, 10000).unwrap(), Stop::Budget);
        assert_eq!(machine.dispatches(), 10000);
    }
}

#[test]
fn scans_are_bounded_and_match_literal_execution() {
    compare("[>>]", &[1, 9, 2, 9, 3, 9, 0], false, false);
    compare(">>>>>>[<<]", &[0, 9, 2, 9, 3, 9, 1], false, false);
    // Include an excursion beyond the final scan stride in its bounds guard.
    compare("[>><]", &[1, 1, 0], false, false);
    let program = Program::parse("[>]", true, false).unwrap();
    let mut machine = Machine::new(4000).unwrap();
    machine.tape_mut().fill(1);
    machine.tape_mut()[3999] = 0;
    assert_eq!(machine.run(&program, 1).unwrap(), Stop::Budget);
    assert_eq!(machine.pointer(), SCAN_LIMIT);
    assert_eq!(event(&mut machine, &program).unwrap(), Stop::Halted);
    assert_eq!(machine.pointer(), 3999);
}

#[test]
fn input_output_and_host_hooks_are_mutation_barriers() {
    compare("+?+!+$.,>.,<.", &[0, 0, 0], false, true);
    compare("+?+!+$.", &[0, 0, 0], false, false);
    let program = Program::parse(",.", true, true).unwrap();
    let mut machine = Machine::new(1).unwrap();
    assert_eq!(machine.run(&program, 1).unwrap(), Stop::Input);
    assert_eq!(machine.run(&program, 1000).unwrap(), Stop::Input);
    assert_eq!(machine.dispatches(), 1);
    machine.provide_input(128).unwrap();
    assert_eq!(machine.provide_input(0), Err(Error::NoPendingInput));
    assert_eq!(machine.run(&program, 0).unwrap(), Stop::Budget);
    assert_eq!(machine.run(&program, 1).unwrap(), Stop::Output(128));
    assert_eq!(machine.run(&program, 0).unwrap(), Stop::Halted);
}

#[test]
fn even_delta_and_empty_loops_still_obey_budgets() {
    for source in ["+[]", "+[--]", "+[><]"] {
        let program = Program::parse(source, true, false).unwrap();
        let mut machine = Machine::new(3).unwrap();
        assert_eq!(machine.run(&program, 500).unwrap(), Stop::Budget);
        assert_eq!(machine.dispatches(), 500);
    }
}

#[test]
fn randomized_full_state_and_event_differential() {
    let mut seed = 0x12a5_f13du32;
    let mut random = || {
        seed ^= seed << 13;
        seed ^= seed >> 17;
        seed ^= seed << 5;
        seed
    };
    for _ in 0..400 {
        let initial: Vec<u16> = (0..16).map(|_| (random() & 15) as u16).collect();
        let mut source = String::new();
        for _ in 0..8 {
            source.push_str(&">".repeat((random() % 4) as usize));
            source.push_str("[-]");
            source.push_str(&"+".repeat((random() % 12) as usize));
            source.push_str(match random() % 6 {
                0 => "[->+>++<<]",
                1 => "[>---<-]",
                2 => ">+-<++>--<",
                3 => "?+!-$",
                4 => ",.",
                _ => "[-]",
            });
            source.push('.');
        }
        // Some cases intentionally walk out of bounds. Compare partial tape
        // state at the error as well as all successful host-event boundaries.
        compare(&source, &initial, false, true);
        compare(&source, &initial, true, true);
    }
}

#[test]
fn affine_loop_reduces_dispatch_work() {
    let source = "[->+>+++<<]";
    let mut literal = Machine::new(3).unwrap();
    let mut optimized = Machine::new(3).unwrap();
    literal.tape_mut()[0] = 65535;
    optimized.tape_mut()[0] = 65535;
    let plain = Program::parse(source, false, false).unwrap();
    let fast = Program::parse(source, true, false).unwrap();
    assert_eq!(literal.run(&plain, 1_000_000).unwrap(), Stop::Halted);
    assert_eq!(optimized.run(&fast, 1).unwrap(), Stop::Halted);
    assert_eq!(literal.tape(), optimized.tape());
    assert!(literal.dispatches() > 400_000);
    assert_eq!(optimized.dispatches(), 1);
}

#[test]
fn repeated_scans_share_direction_safely_and_keep_stride_residues_separate() {
    for stride in [1, 2, 8] {
        let right = ">".repeat(stride);
        let left = "<".repeat(stride);
        let mut initial = vec![0; stride * 601 + 1];
        for index in 1..601 {
            initial[index * stride] = 1 + (index % 65535) as u16;
        }
        // Adjacent lanes are zero: they must never terminate this residue's scan.
        let round_trip = format!("{right}[{right}]{left}[{left}]");
        compare(&round_trip.repeat(4), &initial, false, false);
        if stride > 1 {
            // A second residue starts at zero although the first is cached nonzero.
            compare(
                &format!("{round_trip}>{right}[{right}]"),
                &initial,
                false,
                false,
            );
        }
    }
}

#[test]
fn cached_scan_runs_observe_zeros_created_by_each_bf_write_form() {
    let cells = 1025;
    let mut initial = vec![1; cells];
    initial[0] = 0;
    initial[cells - 1] = 0;
    let middle = cells / 2;
    for write in ["-", "[-]", "+[-]", ">+<-", "[->+<]"] {
        let source = format!(">[>]{}{}>[>]<[<]", "<".repeat(cells - 1 - middle), write);
        // Warm a long forward run, create an interior zero, then approach it
        // from the other direction. Check complete state against literal BF.
        compare(&source, &initial, false, false);
    }
    // A wrapping increment also creates zero and must invalidate that cell.
    initial[middle] = u16::MAX;
    let source = format!(">[>]{}+>[>]<[<]", "<".repeat(cells - 1 - middle));
    compare(&source, &initial, false, false);
}

#[test]
fn cached_scan_runs_observe_host_mutation_after_hooks_and_budgets() {
    let cells = 1025;
    let source = format!(">[>]${}[>]", "<".repeat(cells - 2));
    for pause_after_move in [false, true] {
        let program = Program::parse(&source, true, true).unwrap();
        let mut actual = Machine::new(cells).unwrap();
        let mut expected = Literal::new(&source, cells, false, true);
        actual.tape_mut().fill(1);
        actual.tape_mut()[0] = 0;
        actual.tape_mut()[cells - 1] = 0;
        expected.tape.copy_from_slice(actual.tape());
        assert_eq!(event(&mut actual, &program).unwrap(), Stop::Hook(b'$'));
        assert_eq!(expected.event().unwrap(), Stop::Hook(b'$'));
        if pause_after_move {
            assert_eq!(actual.run(&program, 1).unwrap(), Stop::Budget);
            assert_eq!(actual.pointer(), 1);
        }
        // Modify a remote, previously nonzero cell through the public mutable
        // view. Both a hook and an ordinary budget return permit this mutation.
        actual.tape_mut()[513] = 0;
        expected.tape[513] = 0;
        assert_eq!(event(&mut actual, &program).unwrap(), Stop::Halted);
        assert_eq!(expected.event().unwrap(), Stop::Halted);
        assert_eq!(actual.pointer(), 513);
        assert_eq!(actual.pointer(), expected.pointer);
        assert_eq!(actual.tape(), expected.tape);
    }
}

#[test]
fn cached_scans_observe_input_zero_and_later_removal_of_a_stop_cell() {
    let cells = 1025;
    let middle = 512;
    let source = format!(
        ">[>]{} , >[>]<[<] + [<] >[>]",
        "<".repeat(cells - 1 - middle)
    );
    let program = Program::parse(&source, true, false).unwrap();
    let mut actual = Machine::new(cells).unwrap();
    let mut expected = Literal::new(&source, cells, false, false);
    actual.tape_mut().fill(1);
    actual.tape_mut()[0] = 0;
    actual.tape_mut()[cells - 1] = 0;
    expected.tape.copy_from_slice(actual.tape());
    assert_eq!(event(&mut actual, &program).unwrap(), Stop::Input);
    assert_eq!(expected.event().unwrap(), Stop::Input);
    assert_eq!(actual.pointer(), middle);
    actual.provide_input(0).unwrap();
    expected.tape[expected.pointer] = 0;
    expected.pending_input = false;
    assert_eq!(event(&mut actual, &program).unwrap(), Stop::Halted);
    assert_eq!(expected.event().unwrap(), Stop::Halted);
    assert_eq!(actual.pointer(), cells - 1);
    assert_eq!(actual.pointer(), expected.pointer);
    assert_eq!(actual.tape(), expected.tape);
}

#[test]
fn known_nonzero_runs_do_not_hide_pointer_excursions_or_wrapping_aliases() {
    // Warm a run ending at the last cell, then use a loop whose body travels
    // one cell farther than its stride. The last iteration must trap before
    // reaching its zero predicate, retaining the exact partial state.
    compare(">[>]<<<<[>><]", &[0, 1, 1, 1, 1, 0], false, false);
    compare(">>>>[<]>>>>[<<>]", &[0, 1, 1, 1, 1, 0], false, false);
    compare(">[>]<<<<[>><]", &[0, 1, 1, 1, 1, 0], true, false);
    compare(">>>>[<]>>>>[<<>]", &[0, 1, 1, 1, 1, 0], true, false);
    // Full cycles without zero cannot be turned into a terminating seek.
    for stride in [1, 2, 8] {
        let program = Program::parse(&format!("[{}]", ">".repeat(stride)), true, false).unwrap();
        let mut machine = Machine::with_wrapping_tape(16).unwrap();
        machine.tape_mut().fill(1);
        assert_eq!(machine.run(&program, 500).unwrap(), Stop::Budget);
        assert_eq!(machine.dispatches(), 500);
        assert!(machine.tape().iter().all(|&value| value == 1));
    }
}

#[test]
fn moving_update_loops_preserve_predicate_dependencies_and_fault_state() {
    for (source, tape) in [
        ("[->]", vec![1, 2, 65535, 0]),
        ("[+>]", vec![65535, 1, 2, 0]),
        ("[>+<->]", vec![1, 65535, 5, 0]),
        ("[>>+<]", vec![1, 1, 65535, 0, 0]),
        ("[+>>-<]", vec![1, 2]),
        ("[<+>>]", vec![1, 2]),
        ("[<+>>]", vec![0, 2]),
        (">>>[-<]", vec![0, 3, 2, 1]),
    ] {
        compare(source, &tape, false, false);
    }
    for source in ["+[->+]", "+[>+<->]"] {
        let program = Program::parse(source, true, false).unwrap();
        let mut machine = Machine::with_wrapping_tape(1).unwrap();
        assert_eq!(machine.run(&program, 1000).unwrap(), Stop::Budget);
        assert_eq!(machine.dispatches(), 1000);
    }
}

#[test]
fn scan_cache_handles_nonpower_strides_residue_collisions_and_stride_changes() {
    for stride in [37usize, 64, 65] {
        let residues: &[usize] = if stride == 65 { &[0, 32, 64] } else { &[0, 32] };
        let mut initial = vec![1; stride * 81 + 65];
        let mut source = String::new();
        let mut pointer = 0;
        for _ in 0..3 {
            for (index, &residue) in residues.iter().enumerate() {
                let stop = stride * (80 - index * 23) + residue;
                initial[residue] = 0;
                initial[stop] = 0;
                let start = stride + residue;
                if start >= pointer {
                    source.push_str(&">".repeat(start - pointer));
                } else {
                    source.push_str(&"<".repeat(pointer - start));
                }
                source.push_str(&format!(
                    "[{}].{}[{}].",
                    ">".repeat(stride),
                    "<".repeat(stride),
                    "<".repeat(stride)
                ));
                pointer = residue;
            }
        }
        compare(&source, &initial, false, false);
    }
    let mut initial = vec![1; 3151];
    initial[0] = 0;
    initial[3150] = 0;
    let mut source = String::new();
    for stride in [3, 1, 5, 2, 9, 7, 10, 3, 1] {
        let right = ">".repeat(stride);
        let left = "<".repeat(stride);
        source.push_str(&format!("{right}[{right}].{left}[{left}]."));
    }
    compare(&source, &initial, false, false);
}

#[test]
fn cached_long_runs_still_trap_on_the_last_body_excursion() {
    let last = 4096;
    let mut initial = vec![1; last + 1];
    initial[0] = 0;
    initial[last] = 0;
    for wrapping in [false, true] {
        compare(
            &format!(">[>]{}[>><]", "<".repeat(last - 1)),
            &initial,
            wrapping,
            false,
        );
        compare(
            &format!("{}[<]{}[<<>]", ">".repeat(last - 1), ">".repeat(last - 1)),
            &initial,
            wrapping,
            false,
        );
    }
    for hole in [1, 256, 512, 768, 1023, 4095] {
        compare(
            &format!(">[>]{}[-]>[>]<[<]", "<".repeat(last - hole)),
            &initial,
            false,
            false,
        );
    }
}

#[test]
fn warm_scan_reuses_a_large_run_without_unbounded_host_work() {
    let cells = 32769;
    let source = format!(">[>]${}[>]$", "<".repeat(cells - 2));
    let program = Program::parse(&source, true, true).unwrap();
    let mut machine = Machine::new(cells).unwrap();
    machine.tape_mut().fill(1);
    machine.tape_mut()[0] = 0;
    machine.tape_mut()[cells - 1] = 0;
    assert_eq!(event(&mut machine, &program).unwrap(), Stop::Hook(b'$'));
    let cold_dispatches = machine.dispatches();
    assert!(
        cold_dispatches > 100,
        "the first scan must yield while learning its run"
    );
    assert_eq!(machine.run(&program, 0).unwrap(), Stop::Budget);
    assert_eq!(event(&mut machine, &program).unwrap(), Stop::Hook(b'$'));
    assert!(
        machine.dispatches() - cold_dispatches <= 4,
        "known nonzero cells should be skipped as one bounded cache lookup"
    );
    assert_eq!(machine.pointer(), cells - 1);
    assert_eq!(machine.tape()[0], 0);
    assert_eq!(machine.tape()[cells - 1], 0);
    assert!(machine.tape()[1..cells - 1].iter().all(|&cell| cell == 1));
}

fn marker_search(stride: isize) -> String {
    let right = if stride > 0 {
        ">".repeat(stride as usize)
    } else {
        "<".repeat(stride.unsigned_abs())
    };
    let left = if stride > 0 {
        "<".repeat(stride as usize)
    } else {
        ">".repeat(stride.unsigned_abs())
    };
    format!("[{right}[{left}-]{left}[-{right}+{left}]{right}]")
}

#[test]
fn moving_marker_search_matches_independent_literal_for_guards_and_boundaries() {
    for stride in [-8isize, -3, -1, 1, 3, 8] {
        for zeros in [0, 1, 7, 300] {
            let start = if stride < 0 { 2500 } else { 16 };
            let target = (start as isize + stride * (zeros + 1)) as usize;
            let source = format!("{}{}", ">".repeat(start), marker_search(stride));
            for value in [1, 2, 32768, 65535] {
                let mut initial = vec![0; 2600];
                initial[start] = 1;
                initial[target] = value;
                compare(&source, &initial, false, false);
            }
        }
        for (marker, trailing, gap) in [(0, 0, 0), (2, 1, 0), (65535, 1, 0), (1, 3, 1)] {
            let start = 24;
            let mut initial = vec![0; 64];
            initial[start] = marker;
            initial[(start as isize - stride) as usize] = trailing;
            initial[(start as isize + stride * (gap + 1)) as usize] = 7;
            let source = format!("{}{}", ">".repeat(start), marker_search(stride));
            compare(&source, &initial, false, false);
        }
        for wrapping in [false, true] {
            let start = if stride < 0 { 0 } else { 16 };
            let mut initial = vec![0; 17];
            initial[start] = 1;
            initial[(start as isize + stride).rem_euclid(17) as usize] = 7;
            compare(
                &format!("{}{}", ">".repeat(start), marker_search(stride)),
                &initial,
                wrapping,
                false,
            );
        }
    }
    // Every offset aliases on a one-cell wrapping tape. The optimized guard
    // must fall back rather than assuming a distinct zero trailing cell.
    for marker in [0, 1, 2, 65535] {
        compare(&marker_search(1), &[marker], true, false);
    }
}

#[test]
fn moving_marker_writes_invalidate_previously_cached_nonzero_cells() {
    let mut initial = vec![1; 602];
    initial[0] = 0;
    initial[601] = 0;
    // Clear the old run's upper endpoint, consume its neighboring marker in a
    // search, then approach that new zero through the cached interval.
    compare(
        &format!(">[>]<[-]<{}{}[>]", marker_search(-1), "<".repeat(598)),
        &initial,
        false,
        false,
    );
    // Mirror it so zero invalidation must retain/shrink the other cache side.
    compare(
        &format!(
            ">[>]{}[-]>{}{}[<]",
            "<".repeat(600),
            marker_search(1),
            ">".repeat(598)
        ),
        &initial,
        false,
        false,
    );
}

#[test]
fn isolated_host_writes_preserve_unrelated_scan_facts_and_observe_new_zeros() {
    let cells = 32769;
    let source = format!(">[>]${}[>]$", "<".repeat(cells - 2));
    let program = Program::parse(&source, true, true).unwrap();
    for zero in [false, true] {
        let mut machine = Machine::new(cells).unwrap();
        machine.tape_mut().fill(1);
        machine.write_cell(0, 0);
        machine.write_cell(cells - 1, 0);
        assert_eq!(event(&mut machine, &program).unwrap(), Stop::Hook(b'$'));
        let before = machine.dispatches();
        machine.write_cell(16384, if zero { 0 } else { 65535 });
        assert_eq!(event(&mut machine, &program).unwrap(), Stop::Hook(b'$'));
        assert_eq!(machine.pointer(), if zero { 16384 } else { cells - 1 });
        if !zero {
            assert!(
                machine.dispatches() - before <= 4,
                "unrelated cached facts were discarded"
            );
        }
    }
}
