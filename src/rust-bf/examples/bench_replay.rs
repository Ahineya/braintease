//! Repeatable native-core benchmark for terminating BFHOST CPU fixtures.
//! No device emulation is performed: use fixtures that do not require responses.
//! Usage: bench_replay FILE OUTPUT_PREFIX [REPEATS=5]
//! Save this binary before changing the engine, then compare baseline/candidate
//! `.trace` files with cmp. They encode exact full tape state at every event as
//! changes from the preceding snapshot, plus each event and pointer. Trace
//! generation and comparison are excluded from reported execution timings.
use bf_wasm::engine::{Machine, Program, Stop};
use std::{
    error::Error,
    fs,
    path::PathBuf,
    time::{Duration, Instant},
};

type Result<T> = std::result::Result<T, Box<dyn Error>>;

#[derive(Debug, PartialEq, Eq)]
struct Event {
    kind: u8,
    value: u8,
    pointer: usize,
    cell: u16,
}

struct Outcome {
    machine: Machine,
    events: Vec<Event>,
}

fn snapshot(bytes: &mut Vec<u8>, previous: &mut [u16], machine: &Machine, event: &Event) {
    bytes.extend_from_slice(&[event.kind, event.value]);
    bytes.extend_from_slice(&(event.pointer as u64).to_le_bytes());
    let count_offset = bytes.len();
    bytes.extend_from_slice(&0u64.to_le_bytes());
    let mut changes = 0u64;
    for (index, (old, &new)) in previous.iter_mut().zip(machine.tape()).enumerate() {
        if *old != new {
            bytes.extend_from_slice(&(index as u64).to_le_bytes());
            bytes.extend_from_slice(&new.to_le_bytes());
            *old = new;
            changes += 1;
        }
    }
    bytes[count_offset..count_offset + 8].copy_from_slice(&changes.to_le_bytes());
}

fn execute(program: &Program, cells: usize, mut trace: Option<&mut Vec<u8>>) -> Result<Outcome> {
    let mut machine = Machine::new(cells)?;
    let mut previous = trace.as_ref().map(|_| vec![0u16; cells]);
    let mut events = Vec::new();
    let deadline = Instant::now() + Duration::from_secs(20);
    loop {
        if Instant::now() >= deadline {
            return Err("benchmark fixture exceeded 20 seconds".into());
        }
        if machine.dispatches() >= 3_000_000_000 {
            return Err("benchmark fixture exceeded dispatch budget".into());
        }
        let stop = machine.run(program, 1_000_000)?;
        let (kind, value) = match stop {
            Stop::Budget => continue,
            Stop::Halted => (0, 0),
            Stop::Input => (1, 0), // EOF is explicit and identical across runs.
            Stop::Output(value) => (2, value),
            Stop::Hook(marker) => (3, marker),
        };
        let event = Event {
            kind,
            value,
            pointer: machine.pointer(),
            cell: machine.tape()[machine.pointer()],
        };
        if let Some(bytes) = trace.as_mut() {
            snapshot(bytes, previous.as_mut().unwrap(), &machine, &event);
        }
        events.push(event);
        match stop {
            Stop::Halted => return Ok(Outcome { machine, events }),
            Stop::Input => machine.provide_input(0)?,
            _ => {}
        }
    }
}

fn run() -> Result<()> {
    let mut args = std::env::args().skip(1);
    let source_path = args
        .next()
        .ok_or("usage: bench_replay FILE OUTPUT_PREFIX [REPEATS=5]")?;
    let prefix = PathBuf::from(args.next().ok_or("missing OUTPUT_PREFIX")?);
    let repeats: usize = args.next().map(|arg| arg.parse()).transpose()?.unwrap_or(5);
    if !(1..=20).contains(&repeats) || args.next().is_some() {
        return Err("REPEATS must be between 1 and 20".into());
    }
    let source = fs::read_to_string(source_path)?;
    let (header, body) = source
        .split_once("END_BFHOST")
        .ok_or("expected a BFHOST image")?;
    let words: Vec<_> = header.split_whitespace().collect();
    let at = words
        .iter()
        .position(|&word| word == "TAPE_CELLS")
        .ok_or("missing TAPE_CELLS")?;
    let cells: usize = words.get(at + 1).ok_or("missing tape length")?.parse()?;
    if cells == 0 || cells > 64_000_000 {
        return Err("fixture tape must be within 1..64000000 cells".into());
    }
    let parse_started = Instant::now();
    let program = Program::parse(body, true, true)?;
    let parse_seconds = parse_started.elapsed().as_secs_f64();
    let mut trace = b"BFTRACE1".to_vec();
    trace.extend_from_slice(&(cells as u64).to_le_bytes());
    let reference = execute(&program, cells, Some(&mut trace))?;
    if let Some(parent) = prefix.parent() {
        fs::create_dir_all(parent)?;
    }
    fs::write(prefix.with_extension("trace"), &trace)?;

    let mut times = Vec::new();
    for _ in 0..repeats {
        let started = Instant::now();
        let outcome = execute(&program, cells, None)?;
        times.push(started.elapsed().as_secs_f64());
        // Compare outside the measured interval, including each observable stop.
        assert_eq!(
            outcome.machine.tape(),
            reference.machine.tape(),
            "final tape changed between repetitions"
        );
        assert_eq!(
            outcome.machine.pointer(),
            reference.machine.pointer(),
            "final pointer changed"
        );
        assert_eq!(outcome.events, reference.events, "event stream changed");
        assert_eq!(outcome.machine.dispatches(), reference.machine.dispatches());
    }
    times.sort_by(f64::total_cmp);
    let json = format!("{{\"repeats\":{repeats},\"seconds_median\":{},\"seconds_min\":{},\"seconds_max\":{},\"parse_seconds\":{parse_seconds},\"dispatches\":{},\"events\":{},\"tape_cells\":{cells},\"final_pointer\":{},\"trace_bytes\":{},\"ir_instructions\":{},\"optimized_regions\":{}}}\n",
        times[repeats / 2], times[0], times[repeats - 1], reference.machine.dispatches(), reference.events.len(), reference.machine.pointer(), trace.len(), program.instruction_count(), program.optimization_count());
    fs::write(prefix.with_extension("json"), &json)?;
    print!("{json}");
    Ok(())
}

fn main() {
    if let Err(error) = run() {
        eprintln!("bench_replay: {error}");
        std::process::exit(1);
    }
}
