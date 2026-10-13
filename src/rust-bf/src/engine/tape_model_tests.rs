//! An ordinary Vec is the oracle. Every summarized assignment is offered only
//! after checking its claimed previous value across the complete progression.
use super::*;

fn check(tape: &Tape, expected: &[u16]) {
    assert_eq!(tape.len(), expected.len());
    for (index, &value) in expected.iter().enumerate() {
        assert_eq!(tape[index], value, "logical cell {index}");
    }
    for (slot, run) in tape.runs.iter().enumerate() {
        assert!(run.step > 0 && run.low <= run.high && run.high < expected.len());
        assert_eq!((run.high - run.low) % run.step, 0);
        for index in (run.low..=run.high).step_by(run.step) {
            assert_eq!(expected[index], run.value);
            if let Some(backing) = run.backing {
                assert_eq!(
                    tape.cells[index], backing,
                    "incorrect backing proof at {index}"
                );
            }
            assert!(
                tape.runs[slot + 1..]
                    .iter()
                    .all(|other| !other.contains(index)),
                "overlapping logical runs at {index}"
            );
        }
    }
}

fn assign(
    tape: &mut Tape,
    expected: &mut [u16],
    first: usize,
    last: usize,
    step: usize,
    value: u16,
) {
    let low = first.min(last);
    let high = first.max(last);
    assert!(step > 0 && (high - low) % step == 0);
    let old = expected[first];
    assert!(
        (low..=high)
            .step_by(step)
            .all(|index| expected[index] == old),
        "fixture violated uniform-old proof"
    );
    if !tape.assign_uniform(first, last, step, old, value) {
        // Rejection may not partially change logical state. The ordinary caller
        // then performs the same writes through the pointwise fallback.
        check(tape, expected);
        for index in (low..=high).step_by(step) {
            tape[index] = value;
        }
    }
    for index in (low..=high).step_by(step) {
        expected[index] = value;
    }
    check(tape, expected);
}

#[test]
fn cancellation_after_partial_materialization_cannot_reuse_an_old_backing_proof() {
    let mut expected = vec![0; 4096];
    let mut tape = Tape::new(expected.clone());
    assign(&mut tape, &mut expected, 0, 4095, 1, 1);
    assert!(!tape.runs.is_empty(), "fixture must create a deferred run");
    tape.materialize_step();
    assert_eq!(
        tape.cells.iter().filter(|&&value| value == 1).count(),
        MATERIALIZE_LIMIT
    );
    check(&tape, &expected);
    // Prefix backing is now one; the suffix backing is still zero. Clearing
    // both cannot simply discard the suffix overlay and assume backing zero.
    assign(&mut tape, &mut expected, 4095, 0, 1, 0);
    assert_eq!(tape.materialize(), expected);
}

#[test]
fn overlapping_reassignments_preserve_mixed_backings_and_physical_holes() {
    let mut expected: Vec<_> = (0..512).map(|i| if i < 256 { 7 } else { 11 }).collect();
    let mut tape = Tape::new(expected.clone());
    assign(&mut tape, &mut expected, 0, 255, 1, 5);
    assign(&mut tape, &mut expected, 256, 511, 1, 5);
    assert_eq!(
        tape.runs.len(),
        2,
        "equal values with unequal backings cannot merge"
    );
    assign(&mut tape, &mut expected, 128, 383, 1, 9);
    assert!(tape.runs.iter().any(|run| run.backing.is_none()));
    assign(&mut tape, &mut expected, 383, 128, 1, 5);
    tape[173] = 5; // Equal logical value, but a newly materialized backing cell.
    expected[173] = 5;
    check(&tape, &expected);
    assign(&mut tape, &mut expected, 0, 511, 1, 7);
    assert_eq!(tape.materialize(), expected);
    assert!(expected.iter().all(|&value| value == 7));
}

#[test]
fn incompatible_lattices_reject_atomically_and_bounded_materialization_preserves_them() {
    let mut expected = vec![7; 2048];
    let mut tape = Tape::new(expected.clone());
    assign(&mut tape, &mut expected, 0, 1536, 3, 0);
    // Fill the other residues physically, preserving a different backing for
    // the old progression while making the whole interval logically uniform.
    for index in 0..=1536 {
        if index % 3 != 0 {
            tape[index] = 0;
            expected[index] = 0;
        }
    }
    check(&tape, &expected);
    assert!(!tape.assign_uniform(0, 1536, 1, 0, 9));
    check(&tape, &expected);
    assign(&mut tape, &mut expected, 0, 1536, 1, 9);
    while !tape.runs.is_empty() {
        tape.materialize_step();
        check(&tape, &expected);
    }
    assert_eq!(tape.cells, expected);
}

struct Random(u32);
impl Random {
    fn next(&mut self) -> usize {
        self.0 ^= self.0 << 13;
        self.0 ^= self.0 >> 17;
        self.0 ^= self.0 << 5;
        self.0 as usize
    }
    fn value(&mut self) -> u16 {
        [0, 1, 7, 11, 65535, self.next() as u16][self.next() % 6]
    }
}

#[test]
fn randomized_uniform_proofs_splits_cancellation_and_materialization_match_vec() {
    for seed in [0x5783_1f69, 0x9307_c431, 0xb3df_5711, 0x182d_797b] {
        let mut random = Random(seed);
        let mut expected: Vec<_> = (0..4096)
            .map(|index| [0, 7, 65535, 11][index / 1024])
            .collect();
        let mut tape = Tape::new(expected.clone());
        for round in 0..400 {
            match random.next() % 8 {
                0 => {
                    let index = random.next() % expected.len();
                    let value = random.value();
                    tape[index] = value;
                    expected[index] = value;
                }
                1 | 2 => {
                    // Establish a proved-old progression by real point writes.
                    // This can punch holes through overlays of any other stride.
                    let step = 1 + random.next() % 11;
                    let low = random.next() % (expected.len() / 2);
                    let count =
                        1 + random.next() % ((expected.len() - 1 - low) / step + 1).min(257);
                    let high = low + (count - 1) * step;
                    let old = random.value();
                    for index in (low..=high).step_by(step) {
                        tape[index] = old;
                        expected[index] = old;
                    }
                    let value = random.value();
                    assign(&mut tape, &mut expected, high, low, step, value);
                }
                3 | 4 if !tape.runs.is_empty() => {
                    let run = tape.runs[random.next() % tape.runs.len()];
                    let first = random.next() % run.count();
                    let count = 1 + random.next() % (run.count() - first);
                    let low = run.low + first * run.step;
                    let high = low + (count - 1) * run.step;
                    // Returning to a known backing value exercises cancellation;
                    // a different value tests propagation through partial splits.
                    let value = if round % 2 == 0 {
                        run.backing.unwrap_or(0)
                    } else {
                        random.value()
                    };
                    assign(&mut tape, &mut expected, low, high, run.step, value);
                }
                5 => {
                    tape.materialize_step();
                }
                6 => {
                    assert_eq!(tape.materialize(), expected);
                }
                _ => {
                    // Find an already uniform prefix without changing physical
                    // cells first, including runs with mixed old backing proofs.
                    let step = 1 + random.next() % 7;
                    let first = random.next() % expected.len();
                    let old = expected[first];
                    let mut last = first;
                    while last + step < expected.len() && expected[last + step] == old {
                        last += step;
                    }
                    let value = random.value();
                    assign(&mut tape, &mut expected, last, first, step, value);
                }
            }
            check(&tape, &expected);
            while tape.needs_materialization() {
                tape.materialize_step();
                check(&tape, &expected);
            }
        }
        assert_eq!(tape.materialize(), expected);
    }
}
