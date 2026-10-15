//! Bounded correctness check of the original compiled C square demo.
//! Run after scripts/run-bf-rgb565-c.sh --build-only. No window or Doom.
use bf_wasm::engine::Program;
use rbf::{
    devices::BfDevices,
    image::Layout,
    runner::{Host, RunOptions},
};
use std::{path::PathBuf, time::Duration};

fn main() -> Result<(), String> {
    let path = std::env::args_os()
        .nth(1)
        .map(PathBuf::from)
        .unwrap_or_else(|| "build/bf/rgb565-demo.bf".into());
    let source = std::fs::read_to_string(path).map_err(|e| e.to_string())?;
    let (layout, body) = Layout::parse(&source, 64_000_000)?;
    if layout.bank_size != 16384 || layout.memory_banks != 3 {
        return Err(
            "Expected the original C demo with 16384-cell banks and three RAM banks".into(),
        );
    }
    let program = Program::parse(body, true, true).map_err(|e| e.to_string())?;
    let make_host = || {
        Host::new(
            layout.clone(),
            BfDevices::new(layout.bank_size, 35, 1, None)?,
        )
    };
    let mut plain = make_host()?;
    let mut native = make_host()?;
    // This legacy demo polls held keys without NEXT_TICK. Set live held state
    // between flushes rather than replaying tick events it will never request.
    let keys = [None, Some(79), Some(81), Some(80), Some(82)];
    let positions = [(30, 30), (31, 30), (31, 31), (30, 31), (30, 30)];
    let mut previous = None;
    let mut compiled = 0;
    for (i, (key, (x, y))) in keys.into_iter().zip(positions).enumerate() {
        for host in [&mut plain, &mut native] {
            let shared = host.devices.display.get_state();
            let mut state = shared.lock().unwrap();
            if let Some(key) = previous {
                state.set_key(key, false);
            }
            if let Some(key) = key {
                state.set_key(key, true);
            }
        }
        previous = key;
        let mut outputs = [Vec::new(), Vec::new()];
        for (index, host) in [&mut plain, &mut native].into_iter().enumerate() {
            let options = RunOptions {
                jit: index == 1,
                stop_at_frame: Some(i as u64 + 1),
                timeout: Some(Duration::from_secs(30)),
                max_instructions: Some(200_000),
                ..RunOptions::default()
            };
            if host.run(&program, &options, &mut outputs[index])? != "checkpoint_frame" {
                return Err("Stopped before requested frame".into());
            }
        }
        let a = plain.devices.display.snapshot().ok_or("Missing frame")?;
        let b = native
            .devices
            .display
            .snapshot()
            .ok_or("Missing native frame")?;
        if a != b || outputs[0] != outputs[1] || plain.instructions != native.instructions {
            return Err(format!("Interpreter/JIT mismatch at frame {}", i + 1));
        }
        if a.width != 64 || a.height != 64 {
            return Err("Unexpected resolution".into());
        }
        for row in 0..64 {
            for column in 0..64 {
                let expected_red = (x..x + 6).contains(&column) && (y..y + 6).contains(&row);
                if (a.pixels[row * 64 + column] == 0xf800) != expected_red {
                    return Err(format!(
                        "Wrong player position at frame {} ({column}, {row})",
                        i + 1
                    ));
                }
            }
        }
        // Inspect full logical state too; snapshots deliberately run outside
        // timing and discard derived caches equally in both hosts.
        if plain.machine.pointer() != native.machine.pointer()
            || plain.machine.tape() != native.machine.tape()
        {
            return Err(format!("Tape mismatch at frame {}", i + 1));
        }
        compiled += native.jit_stats.as_ref().map_or(0, |s| s.native_dispatches);
        println!(
            "frame {}: exact pixels/tape/output; player ({x}, {y})",
            i + 1
        );
    }
    if compiled == 0 {
        return Err("No native dispatches were exercised".into());
    }
    Ok(())
}
