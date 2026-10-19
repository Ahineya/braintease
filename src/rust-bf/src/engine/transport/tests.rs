use super::*;
use crate::engine::{tests::compare, Stop};
const CARRY: &str = "[<[->>>>+<<<<]>>>>>]";
const COUNT: &str = "[-[->>>>+<<<<]>>+>>]";
#[test]
fn carries_and_migrating_counts_match_literal_in_both_directions() {
    for source in [CARRY, COUNT] {
        for sign in [false, true] {
            let body: String = source
                .chars()
                .map(|c| {
                    if sign {
                        match c {
                            '>' => '<',
                            '<' => '>',
                            _ => c,
                        }
                    } else {
                        c
                    }
                })
                .collect();
            let start = if sign { 128 } else { 8 };
            let source = format!("{}{body}", ">".repeat(start));
            for n in [0, 1, 2, 7, 19] {
                for value in [0, 1, 7, 31] {
                    let mut initial = vec![0; 160];
                    if body == COUNT
                        || sign
                            && body == COUNT.replace('>', "x").replace('<', ">").replace('x', "<")
                    {
                        initial[start] = n as u16;
                    } else {
                        let d = if sign { -4 } else { 4 };
                        for i in 0..n {
                            initial[(start as isize + i * d) as usize] = 1;
                        }
                        initial[(start as isize + if sign { 1 } else { -1 }) as usize] = value;
                    }
                    compare(&source, &initial, false, false);
                    compare(&source, &initial, true, false);
                }
            }
        }
    }
}
#[test]
fn overlap_order_aliases_and_barriers_are_rejected() {
    for source in [
        "[<[->>>>+<<<<]>>>>>",
        "[<[->>>>+<<<<]>>>>+>]",
        "[-[->>>>+<<<<]>>>>[->>>>+<<<<]]",
        "[<[->>>>+<<<<]!>>>>>]",
        "[<[-->++++<]>>]",
    ] {
        if let Ok(p) = Program::parse(source, true, true) {
            assert!(Transport::parse(&p, 0).is_none(), "{source}");
        }
    }
}
#[test]
fn occupied_cells_wraparound_and_bounds_fall_back_exactly() {
    for cells in [1, 2, 3, 5, 9, 17, 40, 80] {
        for source in [
            CARRY,
            COUNT,
            "[<[->>>>>+<<<<<]>>>>><]",
            "[<[->>>>+<<<<]>>>>>><]",
        ] {
            for n in [0, 1, 3, 15] {
                let mut initial = vec![0; cells];
                initial[0] = n;
                if cells > 1 {
                    initial[1] = 65535;
                }
                compare(source, &initial, false, false);
                // Tiny wrapping tapes can make the retained loop nonterminating.
            }
        }
    }
    for at in [2, 5, 9, 17, 33] {
        let mut initial = vec![0; 100];
        initial[1] = 31;
        for i in (2..70).step_by(4) {
            initial[i] = 1;
        }
        initial[at] = 65535;
        compare(&format!(">>{CARRY}"), &initial, false, false);
    }
}
#[test]
fn long_migrations_resume_with_host_mutation_and_checkpoint() {
    let p = Program::parse(COUNT, true, false).unwrap();
    let mut m = Machine::new(300000).unwrap();
    m.write_cell(0, 65535);
    assert_eq!(m.run(&p, 1).unwrap(), Stop::Budget);
    let mut snapshot = Vec::new();
    m.write_checkpoint(&p, &mut snapshot).unwrap();
    let mut restored = Machine::read_checkpoint(&p, snapshot.as_slice(), 300000).unwrap();
    for machine in [&mut m, &mut restored] {
        machine.write_cell(4 * 90 + 2, 65535);
        while machine.run(&p, 7).unwrap() == Stop::Budget {}
        assert_eq!(machine.pointer(), 4 * 65535);
        assert_eq!(machine.read_cell(4 * 90 + 2), 0);
        assert_eq!(machine.read_cell(4 * 65535), 0);
        assert!(machine.dispatches() < 10000);
    }
    assert_eq!(m.tape(), restored.tape());
}

#[test]
fn overlapping_state_vectors_and_random_holes_match_character_oracle() {
    let step=">>>>>>>>>>>>>>>>[->>>>>>>>+<<<<<<<<]<<<<<<<<[->>>>>>>>+<<<<<<<<]<<<<<<<<[->>>>>>>>+<<<<<<<<]>>+>>>>>>";
    let counted = format!("[-<<<<<<<<{step}>>>>>>>>]");
    for backwards in [false, true] {
        let body: String = counted
            .chars()
            .map(|c| {
                if backwards {
                    match c {
                        '>' => '<',
                        '<' => '>',
                        _ => c,
                    }
                } else {
                    c
                }
            })
            .collect();
        let p = Program::parse(&body, true, false).unwrap();
        assert!(Transport::parse(&p, 0).is_some());
        let start = if backwards { 256 } else { 32 };
        let direction = if backwards { -1 } else { 1 };
        let source = format!("{}{body}", ">".repeat(start));
        let mut seed = 0xabe317u32;
        for _ in 0..160 {
            seed = seed.wrapping_mul(1664525).wrapping_add(1013904223);
            let mut initial = vec![0; 320];
            initial[start] = (seed % 12) as u16;
            initial[(start as isize - 8 * direction) as usize] = ((seed >> 8) % 8) as u16;
            initial[(start as isize + 8 * direction) as usize] = ((seed >> 16) % 8) as u16;
            // Include occupied destinations and wrapping marker increments.
            for (offset, value) in [(24, 1), (34, 65535), (50, 7)] {
                if seed & (1 << ((offset / 8) % 16)) != 0 {
                    initial[(start as isize + offset * direction) as usize] = value;
                }
            }
            compare(&source, &initial, false, false);
        }
    }
}
#[test]
fn state_mutation_after_budget_forces_a_new_zero_proof() {
    for bulk in [false, true] {
        let p = Program::parse(COUNT, true, false).unwrap();
        let mut m = Machine::new(12000).unwrap();
        m.write_cell(0, 2000);
        assert_eq!(m.run(&p, 1).unwrap(), Stop::Budget);
        if bulk {
            m.tape_mut()[400] = 3;
        } else {
            m.write_cell(400, 3);
        }
        while m.run(&p, 1).unwrap() == Stop::Budget {}
        assert_eq!(m.pointer(), 4 * 2003);
        for i in 0..2003 {
            assert_eq!(m.read_cell(i * 4), 0);
            assert_eq!(m.read_cell(i * 4 + 2), 1);
        }
        assert_eq!(m.read_cell(4 * 2003), 0);
    }
}
#[test]
fn warm_transport_skips_only_proven_zero_state_and_invariant_predicates() {
    let source = format!(">>{CARRY}!{}>{CARRY}", "<".repeat(4001));
    let p = Program::parse(&source, true, true).unwrap();
    let mut m = Machine::new(4010).unwrap();
    for i in 0..1000 {
        m.write_cell(2 + 4 * i, 1);
    }
    m.write_cell(1, 65535);
    while m.run(&p, 1).unwrap() == Stop::Budget {}
    assert_eq!(m.pointer(), 4002);
    assert_eq!(m.read_cell(4001), 65535);
    m.write_cell(1, 32768);
    let before = m.dispatches();
    // The final arithmetic movement returns to the original predicate.
    while m.run(&p, 1).unwrap() == Stop::Budget {}
    assert_eq!(m.read_cell(4001), 32767); // 65535 + 32768 modulo 65536.
    assert!(m.dispatches() - before < 100);
}
#[test]
fn full_width_carried_values_and_wrapping_aliases_are_not_assumed_small() {
    for value in [32768, 65535] {
        let mut initial = vec![0; 12];
        initial[1] = value;
        initial[2] = 1;
        compare(&format!(">>{CARRY}"), &initial, false, false);
    }
    for source in [CARRY, COUNT] {
        for optimize in [false, true] {
            let p = Program::parse(source, optimize, false).unwrap();
            let mut m = Machine::with_wrapping_tape(1).unwrap();
            m.write_cell(0, 1);
            assert_eq!(m.run(&p, 1000).unwrap(), Stop::Budget);
        }
    }
}

#[test]
fn cleared_progressions_remain_proven_after_predicate_cache_eviction() {
    for backwards in [false, true] {
        let body: String = COUNT
            .chars()
            .map(|c| {
                if backwards {
                    match c {
                        '>' => '<',
                        '<' => '>',
                        _ => c,
                    }
                } else {
                    c
                }
            })
            .collect();
        let p = Program::parse(&body, true, false).unwrap();
        let mut m = Machine::new(20480).unwrap();
        let start = if backwards { 18000 } else { 16 };
        let direction = if backwards { -1isize } else { 1 };
        m.pointer = start;
        m.write_cell(start, 2048);
        while m.run(&p, 1).unwrap() == Stop::Budget {}
        // The migrating control left an exact zero progression behind it.
        // A different stride evicts the small predicate cache, but should not
        // force every future traversal to discover the same empty cells again.
        m.zero_cache.prepare(3, 1);
        let first = (start as isize + 4 * direction) as usize;
        let proof = m.tape.uniform_prefix(first, 4 * direction, 2047);
        assert_eq!(proof, Some((2047, 0)));
        let hole = (first as isize + 400 * direction) as usize;
        m.write_cell(hole, 9);
        assert_eq!(
            m.tape.uniform_prefix(first, 4 * direction, 2047),
            Some((100, 0))
        );
        assert_eq!(m.read_cell(hole), 9);
        let mut snapshot = Vec::new();
        m.write_checkpoint(&p, &mut snapshot).unwrap();
        let mut restored = Machine::read_checkpoint(&p, snapshot.as_slice(), 20480).unwrap();
        assert_eq!(m.tape(), restored.tape());
    }
}

#[test]
fn long_cleared_ranges_match_literal_with_overlapping_state_and_occupied_holes() {
    let vector="[-<<<<<<<<<<<<<<<<>>>>>>>>>>>>>>>>[->>>>>>>>+<<<<<<<<]<<<<<<<<[->>>>>>>>+<<<<<<<<]<<<<<<<<[->>>>>>>>+<<<<<<<<]>>+>>>>>>>>>>>>>>>>>>>>>>]";
    for source in [COUNT, vector] {
        for backwards in [false, true] {
            let direction = if backwards { -1isize } else { 1 };
            let body: String = source
                .chars()
                .map(|c| {
                    if backwards {
                        match c {
                            '>' => '<',
                            '<' => '>',
                            _ => c,
                        }
                    } else {
                        c
                    }
                })
                .collect();
            let start = if backwards { 2300 } else { 32 };
            for n in [64, 129, 257] {
                let mut initial = vec![0u16; 2400];
                initial[start] = n;
                if source == vector {
                    initial[(start as isize - 8 * direction) as usize] = 3;
                    initial[(start as isize - 16 * direction) as usize] = 5;
                }
                // An occupied cell beyond the first batch must prevent a
                // zero shortcut and must be incorporated by literal execution.
                initial[(start as isize + 800 * direction) as usize] = 2;
                compare(
                    &format!("{}{body}", ">".repeat(start)),
                    &initial,
                    false,
                    false,
                );
            }
        }
    }
}
