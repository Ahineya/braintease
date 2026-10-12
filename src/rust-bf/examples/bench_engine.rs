//! Small synthetic benchmarks. These are not predictions of Ripple/Doom speed.
//! cargo run --release --manifest-path src/rust-bf/Cargo.toml --example bench_engine
use bf_wasm::{
    engine::{Machine, Program, Stop},
    optimize, parse, preprocess_collapse, Interpreter,
};
use std::hint::black_box;
use std::time::{Duration, Instant};

fn median(mut values: Vec<Duration>) -> f64 {
    values.sort();
    values[values.len() / 2].as_secs_f64()
}

fn benchmark(name: &str, source: &str, initial: &[u16], iterations: usize) {
    let old = optimize(parse(&preprocess_collapse(source)));
    let new = Program::parse(source, true, false).unwrap();
    let mut reference = Interpreter::<u16>::new(initial.len(), true, true);
    reference.tape.copy_from_slice(initial);
    reference.run(&old);
    let mut check = Machine::with_wrapping_tape(initial.len()).unwrap();
    check.tape_mut().copy_from_slice(initial);
    while check.run(&new, 1_000_000).unwrap() == Stop::Budget {}
    assert_eq!(reference.tape, check.tape());
    assert_eq!(reference.ptr, check.pointer());
    let mut old_times = Vec::new();
    let mut new_times = Vec::new();
    for round in 0..5 {
        for optimized in if round % 2 == 0 {
            [false, true]
        } else {
            [true, false]
        } {
            let start = Instant::now();
            for _ in 0..iterations {
                if optimized {
                    let mut machine = Machine::with_wrapping_tape(initial.len()).unwrap();
                    machine.tape_mut().copy_from_slice(initial);
                    loop {
                        match machine.run(&new, 1_000_000).unwrap() {
                            Stop::Halted => break,
                            Stop::Budget => {}
                            other => panic!("unexpected benchmark event {other:?}"),
                        }
                    }
                    black_box(machine.tape());
                } else {
                    let mut machine = Interpreter::<u16>::new(initial.len(), true, true);
                    machine.tape.copy_from_slice(initial);
                    machine.run(&old);
                    black_box(&machine.tape);
                }
            }
            if optimized {
                new_times.push(start.elapsed());
            } else {
                old_times.push(start.elapsed());
            }
        }
    }
    let old_seconds = median(old_times);
    let new_seconds = median(new_times);
    println!("{name}: iterations={iterations} old={old_seconds:.6}s new={new_seconds:.6}s ratio={:.2}x dispatches/run={}", old_seconds / new_seconds, check.dispatches());
}

fn main() {
    let mut args = std::env::args().skip(1);
    if let Some(file) = args.next() {
        let cells: usize = args
            .next()
            .expect("usage: bench_engine [FILE TAPE_CELLS]")
            .parse()
            .unwrap();
        let source = std::fs::read_to_string(file).unwrap();
        // Only use a terminating BF CPU fixture with no host-dependent reads.
        // Device markers are ignored by both implementations for this timing.
        benchmark("ripple_bf_cpu", &source, &vec![0; cells], 1);
        return;
    }
    benchmark("multi_target_transfer", "[->+>+++<<]", &[20_000, 0, 0], 100);
    benchmark(
        "offset_arithmetic",
        &">+>++<<-".repeat(4096),
        &[0, 0, 0],
        200,
    );
    let mut tape = vec![1; 8193];
    tape[8192] = 0;
    benchmark("stride_scan", "[>>>>>>>>]", &tape, 1000);
    benchmark("routed_transfer", "[->>[>]>+<<[<]<]", &[20_000, 0, 1, 2, 3, 0, 0], 20);
    let mut fill_tape = vec![0; 1027];
    fill_tape[0] = 1024;
    benchmark("counted_scan_fill", "[->>[>]+[<]<]", &fill_tape, 20);
}
