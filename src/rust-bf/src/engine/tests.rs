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

fn compare(source: &str, initial: &[u16], wrapping: bool, hooks: bool) {
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
