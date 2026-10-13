//! Standalone benchmark driver: links the existing Rust VM without changing it.
//! Arguments: BINARY DISK INPUT_JSON OUTPUT_DIR FRAMES [PROFILE_FRAME ...]
//! Ordinary frames use the production runner. Profiled frame timings are marked
//! separately because the additional per-PC accounting changes execution cost.
use rvm::{capture, devices::InputScript, runner::{self, ClockPolicy, RunOptions}, VM};
use rvm::vm::VMState;
use std::{fs, io::Write, path::PathBuf, time::{Duration, Instant}};

fn main() -> Result<(), String> {
    let args: Vec<_> = std::env::args().skip(1).collect();
    if args.len() < 5 {
        return Err("usage: profile_vm BINARY DISK INPUT_JSON OUTPUT_DIR FRAMES [PROFILE_FRAME ...]".into());
    }
    let out = PathBuf::from(&args[3]);
    fs::create_dir_all(&out).map_err(|e| e.to_string())?;
    let frames: u64 = args[4].parse().map_err(|_| "invalid frame count")?;
    let profile_frames: Vec<u64> = args[5..].iter().map(|s| s.parse())
        .collect::<Result<_, _>>().map_err(|_| "invalid profile frame")?;
    let mut vm = VM::with_options(64000, 16_384_000, Some(args[1].clone().into()));
    vm.load_binary(&fs::read(&args[0]).map_err(|e| e.to_string())?)?;
    vm.host_input_enabled = false;
    vm.set_rng_seed(1);
    vm.devices.set_script(InputScript::from_path(&args[2])?)?;
    let mut metrics = fs::File::create(out.join("frames.jsonl")).map_err(|e| e.to_string())?;
    for frame in 1..=frames {
        let before = vm.executed_instructions;
        let start = Instant::now();
        let profiled = profile_frames.contains(&frame);
        if profiled {
            let mut hits = vec![0_u64; vm.instructions.len()];
            while vm.presented_frames < frame {
                if !matches!(vm.state, VMState::Running) {
                    return Err("guest stopped before profiled frame".into());
                }
                let pc = vm.registers[2] as usize * 64000 + vm.registers[1] as usize;
                *hits.get_mut(pc).ok_or("PC outside instruction image")? += 1;
                vm.step()?;
                if vm.executed_instructions - before > 30_000_000_000 {
                    return Err("profile frame instruction limit exceeded".into());
                }
                if vm.executed_instructions % 8192 == 0 {
                    if start.elapsed() > Duration::from_secs(180) {
                        return Err("profile frame timeout".into());
                    }
                    if let Some(error) = &vm.device_error { return Err(error.clone()); }
                    if vm.devices.overflow_seen() { return Err("input overflow during profile".into()); }
                }
            }
            let mut data = Vec::with_capacity(hits.len() * 8);
            for hit in hits { data.extend_from_slice(&hit.to_le_bytes()); }
            fs::write(out.join(format!("frame-{frame}.hits.u64")), data).map_err(|e| e.to_string())?;
        } else {
            let options = RunOptions {
                clock: ClockPolicy::Unthrottled,
                raw_frame: Some(out.join(format!("frame-{frame}.rgb565"))),
                capture_frame: frame,
                deterministic: true,
                max_instructions: Some(vm.executed_instructions + 30_000_000_000),
                timeout: Some(Duration::from_secs(180)),
                ..Default::default()
            };
            if runner::run(&mut vm, &options)? != "captured_frame" {
                return Err("runner stopped before requested frame".into());
            }
        }
        let seconds = start.elapsed().as_secs_f64();
        let snapshot = vm.rgb565_display.as_ref().and_then(|d| d.snapshot())
            .ok_or("missing completed framebuffer")?;
        if profiled { capture::write_raw(&snapshot, &out.join(format!("frame-{frame}.rgb565")))?; }
        if frame == 1 || frame == frames || profiled {
            capture::write_png(&snapshot, &out.join(format!("frame-{frame}.png")))?;
        }
        let record = format!("{{\"frame\":{frame},\"tick\":{},\"width\":{},\"height\":{},\"instructions\":{},\"seconds\":{seconds:.9},\"profiled\":{profiled}}}",
            vm.devices.tick(), snapshot.width, snapshot.height, vm.executed_instructions - before);
        writeln!(metrics, "{record}").map_err(|e| e.to_string())?;
        metrics.flush().map_err(|e| e.to_string())?;
        eprintln!("{record}");
    }
    Ok(())
}
