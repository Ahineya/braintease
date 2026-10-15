use super::*;
use crate::engine::{FastOp, Program, Stop};

const BODY: &str = "[->[<<<]>[>>>]+>>[<<<]>>>[>>>]<<<<]";

fn source(scale: usize, reverse: bool, extra: &str) -> String {
    let start = if reverse { 95 - 30 } else { 30 } * scale;
    let body: String = BODY
        .chars()
        .map(|c| match (c, reverse) {
            ('>', true) => '<',
            ('<', true) => '>',
            _ => c,
        })
        .flat_map(|c| std::iter::repeat_n(c, if c == '>' || c == '<' { scale } else { 1 }))
        .collect();
    format!("{}{body}{extra}", ">".repeat(start))
}

#[test]
fn specialization_matches_general_proof_at_every_budget_and_host_mutation() {
    for scale in [1, 2, 3, 8] {
        for reverse in [false, true] {
            for count in [0, 1, 3, 7, 65535] {
                let text = source(scale, reverse, "$");
                let fast = Program::parse(&text, true, true).unwrap();
                let mut general = Program::parse(&text, true, true).unwrap();
                let mut specialized = 0;
                for op in &mut general.fast {
                    if let FastOp::Routed(route) = op {
                        specialized += usize::from(route.transfer.take().is_some());
                    }
                }
                assert!(specialized > 0);
                // Checkpoint identity excludes only the derived execution plan.
                assert_eq!(format!("{fast:?}"), format!("{general:?}"));
                let mut a = Machine::new(96 * scale).unwrap();
                let mut b = Machine::new(96 * scale).unwrap();
                let at = |n| if reverse { (95 - n) * scale } else { n * scale };
                for m in [&mut a, &mut b] {
                    for i in (4..=31).step_by(3) {
                        m.write_cell(at(i), 32768);
                    }
                    for i in (2..=17).step_by(3) {
                        m.write_cell(at(i), 65535);
                    }
                    m.write_cell(at(30), count);
                }
                for turn in 0..40 {
                    let budget = [0, 1, 2, 7, 31][turn % 5];
                    let want = a.run(&general, budget);
                    let got = b.run(&fast, budget);
                    assert_eq!(got, want, "scale={scale}, reverse={reverse}, count={count}");
                    assert_eq!(
                        (a.pc, a.pointer, a.dispatches),
                        (b.pc, b.pointer, b.dispatches)
                    );
                    for i in 0..a.tape_len() {
                        assert_eq!(a.read_cell(i), b.read_cell(i));
                    }
                    if !matches!(got, Ok(Stop::Budget)) {
                        break;
                    }
                    if turn == 3 {
                        // Change an exit, a marker and the counter after yielding.
                        for (i, value) in [(20, 65535), (13, 0), (30, 3)] {
                            a.write_cell(at(i), value);
                            b.write_cell(at(i), value);
                        }
                    }
                }
            }
        }
    }
}

#[test]
fn specialization_matches_literal_faults_wrapping_aliases_and_cancelled_excursions() {
    for cells in [1, 2, 3, 29, 30, 31, 32, 33, 34, 35, 64] {
        for wrapping in [false, true] {
            for text in [
                source(1, false, ""),
                source(1, false, "").replacen("[->", "[->><", 1),
            ] {
                let fast = Program::parse(&text, true, false).unwrap();
                let plain = Program::parse(&text, false, false).unwrap();
                let mut a = Machine::allocate(cells, wrapping).unwrap();
                let mut b = Machine::allocate(cells, wrapping).unwrap();
                for m in [&mut a, &mut b] {
                    for i in (4..=31).step_by(3).chain((2..=17).step_by(3)) {
                        if i < cells {
                            m.write_cell(i, 1);
                        }
                    }
                    m.write_cell(30 % cells, 3);
                }
                let finish = |m: &mut Machine, p: &Program| {
                    for _ in 0..32 {
                        let stop = m.run(p, 4096);
                        if !matches!(stop, Ok(Stop::Budget)) {
                            return stop;
                        }
                    }
                    Ok(Stop::Budget)
                };
                let want = finish(&mut a, &plain);
                let got = finish(&mut b, &fast);
                assert_eq!(got, want);
                if !matches!(got, Ok(Stop::Budget)) {
                    assert_eq!(a.pointer, b.pointer);
                    for i in 0..cells {
                        assert_eq!(a.read_cell(i), b.read_cell(i));
                    }
                }
            }
        }
    }
}

#[test]
fn cold_long_routes_fall_back_without_writes_then_match_literal_execution() {
    let text = format!("{BODY}$");
    let fast = Program::parse(&text, true, true).unwrap();
    let plain = Program::parse(&text, false, true).unwrap();
    let mut a = Machine::new(1600).unwrap();
    let mut b = Machine::new(1600).unwrap();
    for m in [&mut a, &mut b] {
        m.pointer = 1500;
        for i in (4..=1501).step_by(3).chain((2..=17).step_by(3)) {
            m.write_cell(i, 65535);
        }
        m.write_cell(1500, 3);
    }
    let FastOp::Routed(route) = &fast.fast[fast.fast_at[0] as usize] else {
        panic!("expected a routed loop");
    };
    assert!(route.transfer.is_some());
    assert!(!route.execute(&mut b)); // More than 256 cold scan probes.
    assert_eq!((b.pc, b.pointer, b.dispatches), (0, 1500, 0));
    for i in 0..a.tape_len() {
        assert_eq!(a.read_cell(i), b.read_cell(i));
    }
    assert_eq!(a.run(&plain, 100000).unwrap(), Stop::Hook(b'$'));
    assert_eq!(b.run(&fast, 100000).unwrap(), Stop::Hook(b'$'));
    assert_eq!((a.pc, a.pointer), (b.pc, b.pointer));
    for i in 0..a.tape_len() {
        assert_eq!(a.read_cell(i), b.read_cell(i));
    }
}

#[test]
fn mixed_scan_strides_and_odd_wrapping_deltas_match_literals() {
    for (control, count) in [("-", 3), ("+", 65535), ("+++", 65533)] {
        for destination in ["+", "+++", "-"] {
            let text = format!(
                "{}$",
                BODY.replacen("[-", &format!("[{control}"), 1)
                    .replacen("]+", &format!("]{destination}"), 1)
                    .replacen("[>>>]", "[>>>>>>]", 1)
            );
            let fast = Program::parse(&text, true, true).unwrap();
            let plain = Program::parse(&text, false, true).unwrap();
            assert!(Routed::parse(&fast, 0).unwrap().transfer.is_some());
            let mut a = Machine::new(96).unwrap();
            let mut b = Machine::new(96).unwrap();
            for m in [&mut a, &mut b] {
                m.pointer = 30;
                m.write_cell(30, count);
                for i in (4..=31).step_by(3).chain((2..=17).step_by(3)) {
                    m.write_cell(i, 32768);
                }
            }
            assert_eq!(a.run(&plain, 100000).unwrap(), Stop::Hook(b'$'));
            assert_eq!(b.run(&fast, 100000).unwrap(), Stop::Hook(b'$'));
            assert_eq!((a.pc, a.pointer), (b.pc, b.pointer));
            for i in 0..a.tape_len() {
                assert_eq!(a.read_cell(i), b.read_cell(i));
            }
        }
    }
}

#[test]
fn dependent_lanes_nonzero_write_positions_and_barriers_keep_general_execution() {
    for text in [
        BODY.replacen("[->", "[->>>", 1),
        BODY.replacen("+>>", "+>>>", 1),
        BODY.replacen("]+", "]>+", 1),
        BODY.replacen("[-", "[--", 1),
    ] {
        let program = Program::parse(&text, true, false).unwrap();
        let route = Routed::parse(&program, 0).unwrap();
        assert!(route.transfer.is_none(), "{text}");
    }
    for barrier in ["$", "!", "?", ",", "."] {
        let text = BODY.replacen('+', &format!("+{barrier}"), 1);
        let program = Program::parse(&text, true, true).unwrap();
        assert!(Routed::parse(&program, 0).is_none());
    }
}
