//! Opt-in real FUZIX terminal regression and latency probe, using a private disk.
use rvm::{
    display_rgb565::RGB565Display,
    input_events::InputTransition,
    vm::{VMState, VM},
};
use std::{
    path::{Path, PathBuf},
    process::Command,
    time::{Duration, Instant},
};
struct Disk(PathBuf);
impl Drop for Disk {
    fn drop(&mut self) {
        let _ = std::fs::remove_file(&self.0);
    }
}
fn key(vm: &mut VM, code: u16) {
    vm.devices.accept_events([
        InputTransition::Key {
            key: code,
            down: true,
        },
        InputTransition::Key {
            key: code,
            down: false,
        },
    ]);
}
fn text(vm: &mut VM, s: &str) {
    for c in s.chars() {
        key(
            vm,
            if c == ' ' {
                44
            } else {
                rvm::devices::key_code(&c.to_string()).unwrap()
            },
        );
    }
}
fn drive(vm: &mut VM) {
    vm.run_batch(65536).expect("guest execution");
    vm.get_output();
    assert!(!matches!(vm.state, VMState::Error(_)), "{:?}", vm.state);
}
fn font() -> Vec<u16> {
    include_str!("../../ports/graphics/fonts/rom8x16.inc")
        .split("0x")
        .skip(1)
        .map(|s| u16::from_str_radix(&s[..4], 16).unwrap())
        .collect()
}
fn expected(lines: &[String], cursor: (usize, usize)) -> Vec<u16> {
    expected_size(lines, cursor, 80, 30)
}
fn expected_size(lines: &[String], cursor: (usize, usize), cols: usize, rows: usize) -> Vec<u16> {
    let font = font();
    let width = cols * 8;
    let mut pixels = vec![0; width * rows * 16];
    for (y, line) in lines.iter().enumerate() {
        for (x, c) in line.bytes().enumerate() {
            for sy in 0..16 {
                for sx in 0..8 {
                    if font[usize::from(c) * 16 + sy] & (0x8000 >> sx) != 0 {
                        pixels[(y * 16 + sy) * width + x * 8 + sx] = 0xad55;
                    }
                }
            }
        }
    }
    for sx in 0..8 {
        pixels[(cursor.0 * 16 + 15) * width + cursor.1 * 8 + sx] = 0xad55;
    }
    pixels
}

#[test]
#[ignore = "requires built FUZIX images; set FUZIX_TEST_BUILD and run with --ignored --nocapture"]
fn smaller_terminal_geometry_input_and_scrolling() {
    let root = Path::new(env!("CARGO_MANIFEST_DIR")).parent().unwrap();
    let build = std::env::var_os("FUZIX_TEST_BUILD")
        .map(PathBuf::from)
        .unwrap_or_else(|| root.join("build/fuzix"));
    let disk =
        Disk(std::env::temp_dir().join(format!("fuzix-small-terminal-{}.img", std::process::id())));
    assert!(!disk.0.exists());
    assert!(Command::new("node")
        .arg(root.join("ports/fuzix/tools/disk.mjs"))
        .arg(&build)
        .arg(&disk.0)
        .status()
        .unwrap()
        .success());
    let mut vm = VM::with_options(64000, 16384000, Some(disk.0.clone()));
    vm.host_input_enabled = false;
    vm.jit_enabled = true;
    vm.devices
        .network
        .configure_host(Some(std::ffi::OsStr::new("off")))
        .unwrap();
    vm.load_binary(&std::fs::read(build.join("bios.bin")).unwrap())
        .unwrap();
    vm.rgb565_display = Some(RGB565Display::new());
    vm.push_input_string("gterm -g 64x20\n");
    await_pixels(&mut vm, &expected_size(&["fuzix# ".into()], (0, 7), 64, 20));
    {
        let state = vm.rgb565_display.as_ref().unwrap().get_state();
        let state = state.lock().unwrap();
        assert_eq!((state.width, state.height), (512, 320));
    }
    text(&mut vm, "abcdefgh");
    await_pixels(
        &mut vm,
        &expected_size(&["fuzix# abcdefgh".into()], (0, 15), 64, 20),
    );
    vm.devices.accept_events([InputTransition::Key {
        key: 224,
        down: true,
    }]);
    key(&mut vm, 24);
    vm.devices.accept_events([InputTransition::Key {
        key: 224,
        down: false,
    }]);
    await_pixels(&mut vm, &expected_size(&["fuzix# ".into()], (0, 7), 64, 20));
    text(&mut vm, "seq 1 25");
    key(&mut vm, 40);
    let mut lines: Vec<String> = (7..=25).map(|n| n.to_string()).collect();
    lines.push("fuzix# ".into());
    await_pixels(&mut vm, &expected_size(&lines, (19, 7), 64, 20));
    vm.devices.accept_events([InputTransition::Close]);
    vm.push_input_string("shutdown\n");
    let start = Instant::now();
    while !matches!(vm.state, VMState::Halted) {
        drive(&mut vm);
        assert!(start.elapsed() < Duration::from_secs(30));
    }
    assert!(Command::new("python3")
        .arg(root.join("ports/fuzix/tools/fsck.py"))
        .arg(&disk.0)
        .status()
        .unwrap()
        .success());
}
fn await_pixels(vm: &mut VM, expected: &[u16]) -> Duration {
    let start = Instant::now();
    loop {
        drive(vm);
        if vm.rgb565_display.as_ref().is_some_and(|d| {
            let shared = d.get_state();
            let state = shared.lock().unwrap();
            state.initialized && state.sequence > 0 && state.front_buffer.as_slice() == expected
        }) {
            return start.elapsed();
        }
        assert!(
            start.elapsed() < Duration::from_secs(60),
            "terminal did not produce expected pixels; frames={} state={:?}",
            vm.presented_frames,
            vm.state
        );
    }
}
#[test]
#[ignore = "requires built FUZIX images; set FUZIX_TEST_BUILD and run with --ignored --nocapture"]
fn terminal_readiness_damage_scrolling_and_latency() {
    let root = Path::new(env!("CARGO_MANIFEST_DIR")).parent().unwrap();
    let build = std::env::var_os("FUZIX_TEST_BUILD")
        .map(PathBuf::from)
        .unwrap_or_else(|| root.join("build/fuzix"));
    let disk =
        Disk(std::env::temp_dir().join(format!("fuzix-graphics-{}.img", std::process::id())));
    assert!(!disk.0.exists(), "private test image already exists");
    assert!(Command::new("node")
        .arg(root.join("ports/fuzix/tools/disk.mjs"))
        .arg(&build)
        .arg(&disk.0)
        .status()
        .unwrap()
        .success());
    let mut vm = VM::with_options(64000, 16384000, Some(disk.0.clone()));
    vm.host_input_enabled = false;
    vm.jit_enabled = true;
    vm.devices
        .network
        .configure_host(Some(std::ffi::OsStr::new("off")))
        .unwrap();
    vm.load_binary(&std::fs::read(build.join("bios.bin")).unwrap())
        .unwrap();
    vm.rgb565_display = Some(RGB565Display::new());
    vm.push_input_string("gterm\n");
    let startup = Instant::now();
    let ready = await_pixels(&mut vm, &expected(&["fuzix# ".into()], (0, 7)));
    println!(
        "terminal ready: {:.3}s; instructions={}; JIT={:?}",
        ready.as_secs_f64(),
        vm.executed_instructions,
        vm.jit_stats()
    );
    let mut line = String::from("fuzix# ");
    let mut latency = Vec::new();
    for c in 'a'..='h' {
        key(&mut vm, rvm::devices::key_code(&c.to_string()).unwrap());
        line.push(c);
        latency.push(
            await_pixels(&mut vm, &expected(&[line.clone()], (0, line.len()))).as_secs_f64()
                * 1000.,
        );
    }
    println!("key-to-exact-frame latency ms: {latency:?}");
    // Ctrl-U, cursor movement, insertion/deletion and the software cursor must
    // repaint exactly the changed cells, leaving all surrounding pixels intact.
    vm.devices.accept_events([InputTransition::Key {
        key: 224,
        down: true,
    }]);
    key(&mut vm, 24);
    vm.devices.accept_events([InputTransition::Key {
        key: 224,
        down: false,
    }]);
    await_pixels(&mut vm, &expected(&["fuzix# ".into()], (0, 7)));
    text(&mut vm, "abc");
    await_pixels(&mut vm, &expected(&["fuzix# abc".into()], (0, 10)));
    key(&mut vm, 80);
    key(&mut vm, 42);
    await_pixels(&mut vm, &expected(&["fuzix# ac".into()], (0, 8)));
    vm.devices.accept_events([InputTransition::Key {
        key: 224,
        down: true,
    }]);
    key(&mut vm, 24);
    vm.devices.accept_events([InputTransition::Key {
        key: 224,
        down: false,
    }]);
    await_pixels(&mut vm, &expected(&["fuzix# ".into()], (0, 7)));
    text(&mut vm, "seq 1 40");
    key(&mut vm, 40);
    let mut lines: Vec<String> = (12..=40).map(|n| n.to_string()).collect();
    lines.push("fuzix# ".into());
    await_pixels(&mut vm, &expected(&lines, (29, 7)));
    // Loss of focus must cancel repeat; a held key starts repeating only after
    // its deadline, independently of output/event frequency.
    vm.devices
        .accept_events([InputTransition::Key { key: 4, down: true }]);
    await_pixels(
        &mut vm,
        &expected(
            &[lines[..29].to_vec(), vec!["fuzix# a".into()]].concat(),
            (29, 8),
        ),
    );
    let settled = expected(
        &[lines[..29].to_vec(), vec!["fuzix# aa".into()]].concat(),
        (29, 9),
    );
    await_pixels(&mut vm, &settled);
    vm.devices
        .accept_events([InputTransition::Focus { focused: false }]);
    let quiet = Instant::now();
    while quiet.elapsed() < Duration::from_millis(650) {
        drive(&mut vm);
        // A final PTY escape-sequence fragment may cause an identical repaint.
        // Check visible contents: a continued repeat must add another letter.
        let state = vm.rgb565_display.as_ref().unwrap().get_state();
        let state = state.lock().unwrap();
        assert_eq!(
            state.front_buffer.as_slice(),
            settled.as_slice(),
            "focus loss failed to cancel repeat"
        );
    }
    vm.devices.accept_events([InputTransition::Close]);
    vm.push_input_string("shutdown\n");
    while !matches!(vm.state, VMState::Halted) {
        drive(&mut vm);
        assert!(startup.elapsed() < Duration::from_secs(120));
    }
    assert!(Command::new("python3")
        .arg(root.join("ports/fuzix/tools/fsck.py"))
        .arg(&disk.0)
        .status()
        .unwrap()
        .success());
    println!("key-to-exact-frame latency ms: {latency:?}");
}
