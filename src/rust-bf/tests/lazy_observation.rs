//! Public-API differential checks for deferred generic BF tape updates.
//! The reference disables all loop summaries. Intermediate observations use
//! single-cell reads so they do not accidentally materialize the optimized tape.
use bf_wasm::engine::{Error, Machine, Program, Stop};

fn movement(delta: isize) -> String {
    if delta >= 0 {
        ">".repeat(delta as usize)
    } else {
        "<".repeat(delta.unsigned_abs())
    }
}

fn fill(stride: isize) -> String {
    format!(
        "[-{}[{}]+[{}]{}]",
        movement(2 * stride),
        movement(stride),
        movement(-stride),
        movement(-stride)
    )
}

fn offset_walk(stride: isize, offset: isize, change: &str) -> String {
    format!(
        "[{}{change}{}]",
        movement(offset),
        movement(stride - offset)
    )
}

/// Create predicate and target runs on different residues, ending at the first
/// predicate. BF itself establishes both runs so the optimized machine can
/// retain uniform facts across every subsequent observable stop.
fn offset_walk_prefix(
    stride: isize,
    offset: isize,
    count: usize,
    initial: u16,
) -> (String, usize, usize) {
    assert_ne!(offset.unsigned_abs() % stride.unsigned_abs(), 0);
    let start = if stride > 0 {
        64
    } else {
        64 + (count + 4) * stride.unsigned_abs()
    };
    let cells = 129 + (count + 4) * stride.unsigned_abs();
    let normalize = match initial {
        0 => "-",
        1 => "",
        7 => "++++++",
        65535 => "--",
        _ => panic!("unsupported fixture value"),
    };
    let source = format!(
        "{}{}{}[{normalize}{}]{}{}{}",
        movement(start as isize + offset),
        fill(stride),
        movement((count as isize + 1) * stride),
        movement(-stride),
        movement(-offset - stride),
        fill(stride),
        movement(2 * stride),
    );
    (source, start, cells)
}

struct Pair {
    fast: Machine,
    plain: Machine,
    optimized: Program,
    literal: Program,
}

impl Pair {
    fn new(source: &str, cells: usize, wrapping: bool) -> Self {
        let machine = || {
            if wrapping {
                Machine::with_wrapping_tape(cells)
            } else {
                Machine::new(cells)
            }
            .unwrap()
        };
        Self {
            fast: machine(),
            plain: machine(),
            optimized: Program::parse(source, true, true).unwrap(),
            literal: Program::parse(source, false, true).unwrap(),
        }
    }

    fn write(&mut self, index: usize, value: u16) {
        self.fast.write_cell(index, value);
        self.plain.write_cell(index, value);
    }

    fn observe(&self) {
        assert_eq!(self.fast.pointer(), self.plain.pointer());
        assert_eq!(self.fast.tape_len(), self.plain.tape_len());
        for index in 0..self.fast.tape_len() {
            assert_eq!(
                self.fast.read_cell(index),
                self.plain.read_cell(index),
                "cell {index}"
            );
        }
    }

    fn snapshot(&mut self) {
        self.observe();
        let pointer = self.fast.pointer();
        let dispatches = self.fast.dispatches();
        assert_eq!(self.fast.tape(), self.plain.tape());
        assert_eq!(self.fast.pointer(), pointer);
        assert_eq!(self.fast.dispatches(), dispatches);
    }

    fn event(machine: &mut Machine, program: &Program) -> Result<Stop, Error> {
        for budget in [1, 7, 31].into_iter().cycle().take(1_000_000) {
            match machine.run(program, budget)? {
                Stop::Budget => {}
                result => return Ok(result),
            }
        }
        panic!("bounded differential fixture did not reach an event");
    }

    fn next(&mut self) -> Result<Stop, Error> {
        let actual = Self::event(&mut self.fast, &self.optimized);
        let expected = Self::event(&mut self.plain, &self.literal);
        assert_eq!(actual, expected);
        self.observe();
        actual
    }

    fn input(&mut self, value: u8) {
        let before = self.fast.dispatches();
        assert_eq!(self.fast.run(&self.optimized, 0).unwrap(), Stop::Input);
        assert_eq!(self.fast.run(&self.optimized, 999).unwrap(), Stop::Input);
        assert_eq!(self.fast.dispatches(), before);
        self.fast.provide_input(value).unwrap();
        self.plain.provide_input(value).unwrap();
        assert_eq!(self.fast.provide_input(0), Err(Error::NoPendingInput));
        self.observe();
    }
}

#[test]
fn deferred_fills_preserve_hooks_io_point_writes_and_explicit_snapshots() {
    let mut seed = 0x519f_d726u32;
    for stride in [-8isize, -3, -1, 1, 3, 8] {
        for materialize in [false, true] {
            let count = 257;
            let start = if stride > 0 {
                32
            } else {
                32 + (count + 3) * stride.unsigned_abs()
            };
            let cells = 65 + (count + 3) * stride.unsigned_abs();
            // Grow a run, inspect/overwrite its first cell through ordinary BF
            // output/input, then walk back through every marker.
            let source = format!(
                "{}{}!{}.,.!{}[-{}]?",
                movement(start as isize),
                fill(stride),
                movement(2 * stride),
                movement((count as isize - 1) * stride),
                movement(-stride)
            );
            let mut pair = Pair::new(&source, cells, false);
            pair.write(start, count as u16);
            assert_eq!(pair.next().unwrap(), Stop::Hook(b'!'));
            for _ in 0..5 {
                seed ^= seed << 13;
                seed ^= seed >> 17;
                seed ^= seed << 5;
                let step = 3 + seed as usize % (count - 2);
                let index = (start as isize + step as isize * stride) as usize;
                pair.write(index, [1, 2, 65535][seed as usize % 3]);
            }
            // Leave the first marker unchanged until BF input overwrites it.
            if materialize {
                pair.snapshot();
            }
            assert_eq!(pair.next().unwrap(), Stop::Output(1));
            assert_eq!(pair.next().unwrap(), Stop::Input);
            pair.input(255);
            assert_eq!(pair.next().unwrap(), Stop::Output(255));
            assert_eq!(pair.next().unwrap(), Stop::Hook(b'!'));
            if materialize {
                let first = (start as isize + 2 * stride) as usize;
                pair.fast.tape_mut()[first] = 0;
                pair.plain.tape_mut()[first] = 0;
            }
            assert_eq!(pair.next().unwrap(), Stop::Hook(b'?'));
            assert_eq!(pair.next().unwrap(), Stop::Halted);
            pair.snapshot();
        }
    }
}

#[test]
fn many_disjoint_runs_and_different_lattices_preserve_each_cell() {
    // More independent progressions than a small bounded overlay cache can
    // retain. A later stride-two walk intersects an earlier stride-one run.
    let mut source = String::new();
    let mut starts = Vec::new();
    let mut pointer = 0isize;
    let mut end = 0usize;
    for group in 0..144 {
        let stride = [1isize, 3, 7][group % 3];
        let start = end + 3;
        source += &movement(start as isize - pointer);
        source += &fill(stride);
        if group % 16 == 15 {
            source.push('!');
        }
        starts.push(start);
        pointer = start as isize;
        end = start + 66 * stride as usize + 3;
    }
    // Writes at overlapping strides, including wrapping a marker from 1 to 0.
    source += &movement(5 - pointer);
    source += "+[>+<>>]$";
    let mut pair = Pair::new(&source, end + 10, false);
    for &start in &starts {
        pair.write(start, 64);
    }
    for (group, &start) in starts.iter().enumerate() {
        if group % 16 == 15 {
            assert_eq!(pair.next().unwrap(), Stop::Hook(b'!'));
            // Host updates split old runs at interior cells without invalidating
            // untouched residues or losing previously deferred writes.
            pair.write(start + 3, 0xffff);
        }
    }
    pair.write(6, 65535);
    assert_eq!(pair.next().unwrap(), Stop::Hook(b'$'));
    assert_eq!(pair.next().unwrap(), Stop::Halted);
    pair.snapshot();
}

#[test]
fn repeated_uniform_walks_wrap_values_without_forcing_raw_tape() {
    for stride in [-3isize, -1, 1, 3] {
        let count = 513usize;
        let start = if stride > 0 {
            8
        } else {
            8 + (count + 3) * stride.unsigned_abs()
        };
        let cells = 17 + (count + 3) * stride.unsigned_abs();
        // 1 -> 65535 while walking back, then 65535 -> 0 walking forward.
        // Every stop is observed logically; the raw tape is requested only
        // after both value-changing passes have completed.
        let source = format!(
            "{}{}!{}[--{}]${}[+{}]?",
            movement(start as isize),
            fill(stride),
            movement((count as isize + 1) * stride),
            movement(-stride),
            movement(stride),
            movement(stride),
        );
        let mut pair = Pair::new(&source, cells, false);
        pair.write(start, count as u16);
        assert_eq!(pair.next().unwrap(), Stop::Hook(b'!'));
        assert_eq!(pair.next().unwrap(), Stop::Hook(b'$'));
        for step in 2..=count + 1 {
            assert_eq!(
                pair.fast
                    .read_cell((start as isize + step as isize * stride) as usize),
                65535
            );
        }
        assert_eq!(pair.next().unwrap(), Stop::Hook(b'?'));
        assert_eq!(pair.next().unwrap(), Stop::Halted);
        pair.snapshot();
    }
}

#[test]
fn arithmetic_wrap_and_bounds_faults_keep_prior_deferred_writes() {
    for stride in [-3isize, -1, 1, 3] {
        for wrapping in [false, true] {
            let count = 257;
            let start = if stride > 0 {
                8
            } else {
                8 + (count + 3) * stride.unsigned_abs()
            };
            let cells = 17 + (count + 3) * stride.unsigned_abs();
            let first = (start as isize + 2 * stride) as usize;
            // The added excursion runs beyond the physical tape before returning
            // to an otherwise valid endpoint. Its partial writes remain visible
            // on strict faults; wrapping execution must still reach the hook.
            let source = format!(
                "{}{}!{}+{}{}?",
                movement(start as isize),
                fill(stride),
                movement(2 * stride),
                movement(cells as isize + 1),
                movement(-(cells as isize + 1))
            );
            let mut pair = Pair::new(&source, cells, wrapping);
            pair.write(start, count as u16);
            assert_eq!(pair.next().unwrap(), Stop::Hook(b'!'));
            pair.write(first, 65535);
            let result = pair.next();
            if wrapping {
                assert_eq!(result.unwrap(), Stop::Hook(b'?'));
                assert_eq!(pair.fast.read_cell(first), 0);
                assert_eq!(pair.next().unwrap(), Stop::Halted);
            } else {
                assert!(matches!(result, Err(Error::TapeBounds { .. })));
                assert_eq!(pair.fast.read_cell(first), 0);
            }
            pair.snapshot();
        }
    }
}

#[test]
fn budget_stops_allow_mutation_without_losing_a_partially_filled_run() {
    let count = 2049usize;
    let source = format!("{}!", fill(3));
    let mut pair = Pair::new(&source, (count + 3) * 3, false);
    pair.write(0, count as u16);
    // A countdown must yield during a long fill; no materialization is needed
    // to discover its exact completed prefix or to inspect a partial tape.
    assert_eq!(pair.fast.run(&pair.optimized, 1).unwrap(), Stop::Budget);
    let remaining = pair.fast.read_cell(0);
    assert!(remaining > 0 && remaining < count as u16);
    // Advance the literal oracle to that complete outer iteration. Its merged
    // '-' is one dispatch and return to pointer zero finishes the body.
    for _ in 0..10_000_000 {
        let previous = pair.plain.pointer();
        assert_eq!(pair.plain.run(&pair.literal, 1).unwrap(), Stop::Budget);
        if previous != 0 && pair.plain.read_cell(0) == remaining && pair.plain.pointer() == 0 {
            break;
        }
    }
    pair.observe();
    let before = pair.fast.dispatches();
    assert_eq!(pair.fast.run(&pair.optimized, 0).unwrap(), Stop::Budget);
    assert_eq!(pair.fast.dispatches(), before);
    // A changed remaining count is legal host mutation at a budget boundary.
    pair.write(0, 3);
    pair.write(6, 2);
    assert_eq!(pair.next().unwrap(), Stop::Hook(b'!'));
    assert_eq!(pair.next().unwrap(), Stop::Halted);
    pair.snapshot();
}

#[test]
fn offset_walks_preserve_disjoint_uniform_values_and_wrapping_arithmetic() {
    for (stride, offset) in [
        (-8isize, -5isize),
        (8, 5),
        (-5, 2),
        (5, -2),
        (-3, -1),
        (3, 1),
    ] {
        for initial in [0u16, 1, 7, 65535] {
            let count = 513;
            let (mut source, start, cells) = offset_walk_prefix(stride, offset, count, initial);
            source += &offset_walk(stride, offset, "+");
            source.push('!');
            source += &movement(-(count as isize) * stride);
            source += &offset_walk(stride, offset, "-");
            source.push('$');
            source += &movement(-stride);
            source += &format!("[-{}]?", movement(-stride));
            let mut pair = Pair::new(&source, cells, false);
            pair.write(start, count as u16);
            pair.write((start as isize + offset) as usize, count as u16);
            for (event, expected) in [(b'!', initial.wrapping_add(1)), (b'$', initial)] {
                assert_eq!(pair.next().unwrap(), Stop::Hook(event));
                for step in 2..=count + 1 {
                    let predicate = (start as isize + step as isize * stride) as usize;
                    let target = (predicate as isize + offset) as usize;
                    assert_eq!(pair.fast.read_cell(predicate), 1);
                    assert_eq!(pair.fast.read_cell(target), expected);
                }
            }
            // Clearing the predicate progression must leave the distinct target
            // progression untouched, even after it wrapped through zero.
            assert_eq!(pair.next().unwrap(), Stop::Hook(b'?'));
            for step in 2..=count + 1 {
                let predicate = (start as isize + step as isize * stride) as usize;
                assert_eq!(pair.fast.read_cell(predicate), 0);
                assert_eq!(
                    pair.fast.read_cell((predicate as isize + offset) as usize),
                    initial
                );
            }
            assert_eq!(pair.next().unwrap(), Stop::Halted);
            pair.snapshot();
        }
    }
}

#[test]
fn offset_walks_recheck_host_writes_input_and_bulk_created_stops() {
    let mut seed = 0x431a_285cu32;
    for (stride, offset) in [(-8isize, -5isize), (8, 5), (-3, 1), (3, -1)] {
        for materialize in [false, true] {
            let count = 257;
            let (mut source, start, cells) = offset_walk_prefix(stride, offset, count, 0);
            let first = start as isize + 2 * stride;
            source.push('!');
            source += &offset_walk(stride, offset, "+");
            source.push('$');
            source += &movement(-(count as isize) * stride + offset);
            source += ".,.!";
            source += &movement(-offset);
            source += &offset_walk(stride, offset, "-");
            source.push('?');
            let mut pair = Pair::new(&source, cells, false);
            pair.write(start, count as u16);
            pair.write((start as isize + offset) as usize, count as u16);
            assert_eq!(pair.next().unwrap(), Stop::Hook(b'!'));
            for _ in 0..5 {
                seed ^= seed << 13;
                seed ^= seed >> 17;
                seed ^= seed << 5;
                let step = 1 + seed as usize % (count - 1);
                let target = (first + step as isize * stride + offset) as usize;
                pair.write(target, [0, 7, 65535][seed as usize % 3]);
            }
            pair.write((first + 19 * stride) as usize, 2);
            if materialize {
                pair.snapshot();
            }
            assert_eq!(pair.next().unwrap(), Stop::Hook(b'$'));
            assert_eq!(pair.next().unwrap(), Stop::Output(1));
            assert_eq!(pair.next().unwrap(), Stop::Input);
            pair.input(255);
            assert_eq!(pair.next().unwrap(), Stop::Output(255));
            assert_eq!(pair.next().unwrap(), Stop::Hook(b'!'));
            let mut stop_step = count / 2;
            pair.write((first + stop_step as isize * stride) as usize, 0);
            pair.write((first + (count / 3) as isize * stride + offset) as usize, 7);
            if materialize {
                stop_step = count / 4;
                let stop = (first + stop_step as isize * stride) as usize;
                let target = (first + (count / 6) as isize * stride + offset) as usize;
                pair.fast.tape_mut()[stop] = 0;
                pair.plain.tape_mut()[stop] = 0;
                pair.fast.tape_mut()[target] = 65535;
                pair.plain.tape_mut()[target] = 65535;
            }
            assert_eq!(pair.next().unwrap(), Stop::Hook(b'?'));
            assert_eq!(
                pair.fast.pointer(),
                (first + stop_step as isize * stride) as usize
            );
            assert_eq!(pair.fast.read_cell((first + offset) as usize), 254);
            assert_eq!(pair.next().unwrap(), Stop::Halted);
            pair.snapshot();
        }
    }
}

#[test]
fn offset_walk_excursions_preserve_partial_faults_and_wrapping_tape() {
    for stride in [-8isize, -3, 3, 8] {
        for wrapping in [false, true] {
            let offset = stride.signum();
            let excursion = 200 * stride.signum();
            let count = 257;
            let (mut source, start, cells) = offset_walk_prefix(stride, offset, count, 0);
            source.push('!');
            // The target and net pointer advance are valid; an intermediate
            // excursion eventually crosses the edge after some completed writes.
            source += &format!(
                "[{}+{}{}]?",
                movement(offset),
                movement(excursion),
                movement(stride - offset - excursion),
            );
            let mut pair = Pair::new(&source, cells, wrapping);
            pair.write(start, count as u16);
            pair.write((start as isize + offset) as usize, count as u16);
            assert_eq!(pair.next().unwrap(), Stop::Hook(b'!'));
            if wrapping {
                assert_eq!(pair.next().unwrap(), Stop::Hook(b'?'));
                assert_eq!(pair.next().unwrap(), Stop::Halted);
            } else {
                assert!(matches!(pair.next(), Err(Error::TapeBounds { .. })));
            }
            assert_eq!(
                pair.fast
                    .read_cell((start as isize + 2 * stride + offset) as usize),
                1
            );
            pair.snapshot();
        }
    }
}

#[test]
fn offset_writes_aliasing_future_predicates_cannot_skip_a_new_zero() {
    for stride in [-8isize, -3, 3, 8] {
        let count = 257;
        let start = if stride > 0 {
            32
        } else {
            32 + (count + 3) * stride.unsigned_abs()
        };
        let cells = 65 + (count + 3) * stride.unsigned_abs();
        let first = start as isize + 2 * stride;
        let source = format!(
            "{}{}!{}{}?",
            movement(start as isize),
            fill(stride),
            movement(2 * stride),
            offset_walk(stride, stride, "-"),
        );
        let mut pair = Pair::new(&source, cells, false);
        pair.write(start, count as u16);
        assert_eq!(pair.next().unwrap(), Stop::Hook(b'!'));
        assert_eq!(pair.next().unwrap(), Stop::Hook(b'?'));
        assert_eq!(pair.fast.pointer(), (first + stride) as usize);
        assert_eq!(pair.fast.read_cell(first as usize), 1);
        assert_eq!(pair.fast.read_cell((first + stride) as usize), 0);
        assert_eq!(pair.fast.read_cell((first + 2 * stride) as usize), 1);
        assert_eq!(pair.next().unwrap(), Stop::Halted);
        pair.snapshot();
    }
}
