use super::*;

fn movement(delta: isize) -> String {
    if delta >= 0 {
        ">".repeat(delta as usize)
    } else {
        "<".repeat(delta.unsigned_abs())
    }
}

#[test]
fn moving_updates_bound_total_work_and_resume_long_walks() {
    for changes in [1, 4, 32, 64] {
        // Iterations touch disjoint groups so the number of updates is visible
        // directly in the tape, independently of the arithmetic summarizer.
        let body = "+>".repeat(changes);
        let program = Program::parse(&format!("[{body}]."), true, false).unwrap();
        let iterations = 1024;
        let stop = iterations * changes;
        let mut machine = Machine::new(stop + 1).unwrap();
        machine.tape_mut()[..stop].fill(1);
        assert_eq!(machine.run(&program, 1).unwrap(), Stop::Budget);
        let completed = SCAN_LIMIT / (changes + 1);
        assert_eq!(machine.pointer(), completed * changes);
        assert_eq!(machine.pc, 0, "resume must recheck the loop predicate");
        assert!(machine.tape()[..machine.pointer()].iter().all(|&v| v == 2));
        assert!(machine.tape()[machine.pointer()..stop]
            .iter()
            .all(|&v| v == 1));
        assert_eq!(machine.tape()[stop], 0);
        assert!(completed * (changes + 1) <= SCAN_LIMIT);
        assert!(completed > 0);

        let mut reached_output = false;
        for _ in 0..iterations {
            match machine.run(&program, 1).unwrap() {
                Stop::Budget => {}
                Stop::Output(value) => {
                    assert_eq!(value, 0);
                    reached_output = true;
                    break;
                }
                other => panic!("unexpected stop during a bounded walk: {other:?}"),
            }
        }
        assert!(reached_output, "long walk did not resume to its zero cell");
        assert_eq!(machine.pointer(), stop);
        assert!(machine.tape()[..stop].iter().all(|&v| v == 2));
        assert_eq!(machine.tape()[stop], 0);
        assert_eq!(machine.run(&program, 1).unwrap(), Stop::Halted);
    }
}

#[test]
fn moving_updates_recheck_predicates_after_budget_mutations() {
    for distance in [0, 3, 5] {
        let program = Program::parse("[->]!", true, true).unwrap();
        let mut machine = Machine::new(1025).unwrap();
        machine.tape_mut()[..1024].fill(1);
        if distance == 5 {
            machine.tape_mut()[SCAN_LIMIT / 2 + 3] = 0;
        }
        assert_eq!(machine.run(&program, 1).unwrap(), Stop::Budget);
        let current = machine.pointer();
        assert_eq!(machine.tape()[current], 1);
        if distance == 5 {
            // A host can also remove an upcoming stop cell before resuming.
            assert_eq!(machine.tape()[current + 3], 0);
            machine.tape_mut()[current + 3] = 1;
        }
        machine.tape_mut()[current + distance] = 0;
        assert_eq!(machine.run(&program, 100).unwrap(), Stop::Hook(b'!'));
        assert_eq!(machine.pointer(), current + distance);
        assert!(machine.tape()[..machine.pointer()].iter().all(|&v| v == 0));
        assert_eq!(machine.tape()[machine.pointer()], 0);
        assert!(machine.tape()[machine.pointer() + 1..1024]
            .iter()
            .all(|&v| v == 1));
    }
}

#[test]
fn randomized_moving_arithmetic_matches_independent_literal_full_state() {
    let mut seed = 0x68d1_925bu32;
    let mut random = || {
        seed ^= seed << 13;
        seed ^= seed >> 17;
        seed ^= seed << 5;
        seed
    };
    for _ in 0..400 {
        let magnitude = (random() % 5 + 1) as isize;
        let stride = if random() & 1 == 0 {
            magnitude
        } else {
            -magnitude
        };
        let start = if stride > 0 { 24 } else { 104 };
        let mut initial: Vec<u16> = (0..129)
            .map(|_| match random() % 5 {
                0 => 0,
                1 => 1,
                2 => u16::MAX,
                _ => random() as u16,
            })
            .collect();
        initial[start] = (random() as u16).max(1);

        // Always update the current cell; additionally write the next predicate,
        // farther future predicates and an already visited cell. These aliases
        // prohibit summing many iterations against the original tape state.
        let mut body = String::from("-");
        let mut offset = 0;
        for target in [
            stride,
            stride * 2,
            -stride,
            stride * (1 + (random() % 3) as isize),
        ] {
            body.push_str(&movement(target - offset));
            body.push_str(match random() % 4 {
                0 => "+",
                1 => "-",
                2 => "+++",
                _ => "---",
            });
            offset = target;
        }
        body.push_str(&movement(stride - offset));
        let source = format!("{}[{body}].", ">".repeat(start));
        // Nonzero net motion guarantees either a zero predicate or a strict
        // boundary fault within a bounded number of iterations. The independent
        // byte-by-byte oracle compares all partial writes on that fault too.
        super::tests::compare(&source, &initial, false, false);
    }
}
