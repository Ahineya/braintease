use super::*;
use crate::engine::{FastOp, Stop};
use std::io::Cursor;

const HEADER: usize = 61;

fn save(machine: &mut Machine, program: &Program) -> Vec<u8> {
    let mut bytes = Vec::new();
    machine.write_checkpoint(program, &mut bytes).unwrap();
    bytes
}

fn restore(bytes: &[u8], program: &Program, cells: usize) -> Machine {
    Machine::read_checkpoint(program, bytes, cells).unwrap()
}

fn same_state(left: &mut Machine, right: &mut Machine) {
    assert_eq!(left.pointer, right.pointer);
    assert_eq!(left.pc, right.pc);
    assert_eq!(left.pending_input, right.pending_input);
    assert_eq!(left.wrapping_tape, right.wrapping_tape);
    assert_eq!(left.tape(), right.tape());
}

fn finish(machine: &mut Machine, program: &Program) -> Vec<Stop> {
    let mut events = Vec::new();
    for _ in 0..100_000 {
        let stop = machine.run(program, 7).unwrap();
        match stop {
            Stop::Budget => continue,
            Stop::Input => machine.provide_input(13).unwrap(),
            Stop::Halted => return events,
            _ => (),
        }
        events.push(stop);
    }
    panic!("fixture did not halt");
}

#[test]
fn mid_execution_checkpoints_preserve_events_and_logical_state() {
    let fixtures = [
        ",>+++[<+>-]<.$+!>++[<+>-]<?.",
        ">+>+>+>+<<<[>]<[-<]$>.",
        ">+>>>>++++<<<<[>[<-]<[->+<]>]$.",
        "++++[->>[>]+[<]<]$>>[>]<[-<]!.",
    ];
    for optimize in [false, true] {
        for source in fixtures {
            let program = Program::parse(source, optimize, true).unwrap();
            // Include initial state, budget yields, inside loops, consumed
            // hooks/output, pending input, and completed execution.
            for prefix in 0..24 {
                let mut original = Machine::new(256).unwrap();
                for _ in 0..prefix {
                    if original.pending_input {
                        original.provide_input(13).unwrap();
                    }
                    original.run(&program, 1).unwrap();
                }
                let count = original.dispatches();
                let mut resumed = restore(&save(&mut original, &program), &program, 256);
                assert_eq!(count, resumed.dispatches());
                same_state(&mut original, &mut resumed);
                assert_eq!(
                    finish(&mut original, &program),
                    finish(&mut resumed, &program)
                );
                same_state(&mut original, &mut resumed);
            }
        }
    }
}

#[test]
fn pending_input_and_consumed_output_hooks_are_not_replayed() {
    let program = Program::parse(",.$", true, true).unwrap();
    let mut machine = Machine::new(10).unwrap();
    assert_eq!(machine.run(&program, 1).unwrap(), Stop::Input);
    let mut machine = restore(&save(&mut machine, &program), &program, 10);
    let count = machine.dispatches();
    assert_eq!(machine.run(&program, 0).unwrap(), Stop::Input);
    assert_eq!(machine.run(&program, 20).unwrap(), Stop::Input);
    assert_eq!(count, machine.dispatches());
    machine.provide_input(254).unwrap();
    assert_eq!(machine.run(&program, 20).unwrap(), Stop::Output(254));
    let mut machine = restore(&save(&mut machine, &program), &program, 10);
    assert_eq!(machine.run(&program, 20).unwrap(), Stop::Hook(b'$'));
    let mut machine = restore(&save(&mut machine, &program), &program, 10);
    assert_eq!(machine.run(&program, 20).unwrap(), Stop::Halted);
    assert_eq!(machine.provide_input(0), Err(Error::NoPendingInput));

    let program = Program::parse(",", true, false).unwrap();
    let mut machine = Machine::new(1).unwrap();
    assert_eq!(machine.run(&program, 1).unwrap(), Stop::Input);
    let mut machine = restore(&save(&mut machine, &program), &program, 1);
    assert_eq!(machine.run(&program, 1).unwrap(), Stop::Input);
    machine.provide_input(1).unwrap();
    assert_eq!(machine.run(&program, 1).unwrap(), Stop::Halted);
}

#[test]
fn deferred_writes_and_pending_maintenance_are_materialized_exactly() {
    let program = Program::parse("+$.", true, true).unwrap();
    let mut machine = Machine::new(40_000).unwrap();
    for run in 0..140 {
        let first = 128 + run * 256;
        assert!(machine
            .tape
            .assign_uniform(first, first + 126, 2, 0, run as u16 + 1));
    }
    assert!(machine.tape.needs_materialization());
    assert_eq!(machine.run(&program, 1).unwrap(), Stop::Budget);
    assert_eq!(machine.pc, 0);
    assert!(machine.tape.needs_materialization());
    let expected = (0..machine.tape_len())
        .map(|at| machine.read_cell(at))
        .collect::<Vec<_>>();
    let count = machine.dispatches();
    let mut resumed = restore(&save(&mut machine, &program), &program, 40_000);
    assert_eq!(resumed.tape(), expected);
    assert_eq!(resumed.dispatches(), count);
    assert!(!machine.tape.needs_materialization());
    assert!(!resumed.tape.needs_materialization());
    assert_eq!(
        finish(&mut machine, &program),
        finish(&mut resumed, &program)
    );
    same_state(&mut machine, &mut resumed);
}

#[test]
fn optimized_fill_checkpoint_resumes_with_cold_caches() {
    let program = Program::parse("[->>[>]+[<]<]$>>[>]<[-<]!", true, true).unwrap();
    let mut machine = Machine::new(65_540).unwrap();
    machine.write_cell(0, 64_000);
    let (slot, _) = machine.zero_cache.prepare(1, 2);
    machine.zero_cache.remember(slot, 2, 65_539);
    assert_eq!(machine.run(&program, 2).unwrap(), Stop::Hook(b'$'));
    assert_eq!(machine.read_cell(64_001), 1);
    let mut resumed = restore(&save(&mut machine, &program), &program, 65_540);
    assert_eq!(
        finish(&mut machine, &program),
        finish(&mut resumed, &program)
    );
    same_state(&mut machine, &mut resumed);
}

#[test]
fn pointer_wrapping_bounds_errors_and_counter_are_preserved() {
    let program = Program::parse("+<+.$", true, true).unwrap();
    let mut machine = Machine::with_wrapping_tape(3).unwrap();
    machine.run(&program, 1).unwrap();
    let mut resumed = restore(&save(&mut machine, &program), &program, 3);
    assert_eq!(
        finish(&mut machine, &program),
        finish(&mut resumed, &program)
    );
    same_state(&mut machine, &mut resumed);
    assert_eq!(resumed.read_cell(2), 1);

    let mut machine = Machine::new(3).unwrap();
    let failure = machine.run(&program, 10).unwrap_err();
    let mut resumed = restore(&save(&mut machine, &program), &program, 3);
    assert_eq!(resumed.run(&program, 10).unwrap_err(), failure);
    machine.dispatches = u64::MAX;
    let mut resumed = restore(&save(&mut machine, &program), &program, 3);
    assert_eq!(resumed.run(&program, 1), Err(Error::DispatchOverflow));
}

#[test]
fn sparse_periodic_dense_and_partial_pages_round_trip_compactly() {
    let program = Program::parse("", true, false).unwrap();
    let cells = 2_000_003;
    let mut machine = Machine::new(cells).unwrap();
    // A long generic repeating pattern, unrelated to any guest layout.
    for index in 3 * PAGE_CELLS..100 * PAGE_CELLS {
        machine.write_cell(index, if index % 17 == 3 { 65_535 } else { 0 });
    }
    // Dense nonperiodic page; sparse isolated cells and a partial final page.
    for index in 0..PAGE_CELLS {
        machine.write_cell(index, (index * 31 + 1) as u16);
    }
    machine.write_cell(PAGE_CELLS + 47, 0x8000);
    machine.write_cell(cells - 1, 0x1234);
    let bytes = save(&mut machine, &program);
    assert!(bytes.len() < 14_000, "{} bytes", bytes.len());
    let mut resumed = restore(&bytes, &program, cells);
    same_state(&mut machine, &mut resumed);
    assert_eq!(resumed.run(&program, 1).unwrap(), Stop::Halted);
}

#[test]
fn malformed_headers_and_different_programs_are_rejected() {
    let program = Program::parse(",+.$", true, true).unwrap();
    let bytes = save(&mut Machine::new(20).unwrap(), &program);
    let invalid = |bytes: &[u8]| assert!(Machine::read_checkpoint(&program, bytes, 20).is_err());
    for offset in [0, 8, 12, 60, HEADER] {
        let mut bad = bytes.clone();
        bad[offset] = 255;
        invalid(&bad);
    }
    for (offset, value) in [(28, 0), (28, 21), (28, u64::MAX), (36, 20), (44, 100)] {
        let mut bad = bytes.clone();
        bad[offset..offset + 8].copy_from_slice(&value.to_le_bytes());
        invalid(&bad);
    }
    let mut pending = bytes.clone();
    pending[60] = 1;
    invalid(&pending);
    pending[44..52].copy_from_slice(&2u64.to_le_bytes());
    invalid(&pending);
    for other in [",-.$", ",+.!"] {
        let other = Program::parse(other, true, true).unwrap();
        assert!(Machine::read_checkpoint(&other, bytes.as_slice(), 20).is_err());
    }
    // Even when a program has no optimizable operations or hook characters,
    // explicit parse options are part of its compatibility identity.
    let minimal = Program::parse("+", true, false).unwrap();
    let bytes = save(&mut Machine::new(1).unwrap(), &minimal);
    for (optimize, hooks) in [(false, false), (true, true)] {
        let other = Program::parse("+", optimize, hooks).unwrap();
        assert!(Machine::read_checkpoint(&other, bytes.as_slice(), 1).is_err());
    }
}

#[test]
fn fingerprint_includes_fast_metadata_not_just_literal_code() {
    let mut program = Program::parse("[->+<]", true, false).unwrap();
    let bytes = save(&mut Machine::new(10).unwrap(), &program);
    if let FastOp::Affine { inverse, .. } = &mut program.fast[0] {
        *inverse = inverse.wrapping_add(1);
    } else {
        panic!("expected affine optimization");
    }
    assert!(Machine::read_checkpoint(&program, bytes.as_slice(), 10).is_err());
}

#[test]
fn invalid_pages_and_truncated_files_are_rejected() {
    let program = Program::parse("+", true, false).unwrap();
    let mut machine = Machine::new(11).unwrap();
    machine.write_cell(2, 42);
    let bytes = save(&mut machine, &program);
    assert_eq!(bytes[HEADER], BITMAP);
    for length in 0..bytes.len() {
        assert!(
            Machine::read_checkpoint(&program, &bytes[..length], 11).is_err(),
            "{length}"
        );
    }
    let mut bad = bytes.clone();
    bad[HEADER + 2] |= 0x80;
    assert!(Machine::read_checkpoint(&program, bad.as_slice(), 11).is_err());
    let mut bad = bytes.clone();
    bad[HEADER + 3..].fill(0);
    assert!(Machine::read_checkpoint(&program, bad.as_slice(), 11).is_err());
    for period in [0, 6, 65, 255] {
        let mut bad = bytes[..HEADER].to_vec();
        bad.extend_from_slice(&[PERIODIC, period]);
        assert!(Machine::read_checkpoint(&program, bad.as_slice(), 11).is_err());
    }
    for count in [0u64, 2, u64::MAX] {
        let mut bad = bytes[..HEADER].to_vec();
        bad.push(ZERO);
        bad.extend_from_slice(&count.to_le_bytes());
        assert!(Machine::read_checkpoint(&program, bad.as_slice(), 11).is_err());
    }
}

#[test]
fn readers_stop_at_machine_boundary_and_write_failures_preserve_state() {
    let program = Program::parse("+$", true, true).unwrap();
    let mut machine = Machine::new(10).unwrap();
    machine.run(&program, 1).unwrap();
    let mut bytes = save(&mut machine, &program);
    let boundary = bytes.len();
    bytes.extend_from_slice(b"host device data");
    let mut reader = Cursor::new(bytes);
    let mut resumed = Machine::read_checkpoint(&program, &mut reader, 10).unwrap();
    assert_eq!(reader.position(), boundary as u64);
    same_state(&mut machine, &mut resumed);
    let mut target = [0u8; HEADER + 1];
    assert!(matches!(
        machine.write_checkpoint(&program, &mut target[..]),
        Err(CheckpointError::Io(_))
    ));
    same_state(&mut machine, &mut resumed);
    assert_eq!(machine.dispatches(), resumed.dispatches());
}
