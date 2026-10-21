//! Real Shikaku input, zoom, resize and persistence on JIT RVM.
use rvm::{
    display_rgb565::RGB565Display,
    input_events::InputTransition,
    vm::{VMState, VM},
};
use std::{
    path::PathBuf,
    time::{Duration, Instant},
};
struct Guest {
    vm: VM,
    log: String,
    output: PathBuf,
}
impl Guest {
    fn drive(&mut self) {
        self.vm.run_batch(65536).expect("guest execution");
        assert!(
            self.vm.device_error.is_none(),
            "device error: {:?}",
            self.vm.device_error
        );
        assert!(
            !self.vm.devices.has_pending_tick(),
            "Unexpected NEXT_TICK in Unix guest"
        );
        let bytes = self.vm.get_output();
        if !bytes.is_empty() {
            self.log.push_str(&String::from_utf8_lossy(&bytes));
            std::fs::write(self.output.join("guest.log"), &self.log).unwrap();
        }
        assert!(
            !matches!(self.vm.state, VMState::Error(_)),
            "{:?}\n{}",
            self.vm.state,
            self.log
        );
        for bad in ["panic:", "GsError", "NXR_BUFFER_FAILED", "FAIL line"] {
            assert!(!self.log.contains(bad), "{}", self.log);
        }
    }
    fn until(&mut self, marker: &str, at: usize) {
        let end = Instant::now() + Duration::from_secs(120);
        while !self.log[at..].contains(marker) {
            assert!(
                Instant::now() < end && !matches!(self.vm.state, VMState::Halted),
                "waiting for {marker}\n{}",
                self.log
            );
            self.drive();
        }
    }
    fn command(&mut self, text: &str, marker: &str) {
        let at = self.log.len();
        self.vm.push_input_string(text);
        self.until(marker, at);
    }
    fn settle(&mut self, ms: u64) {
        let end = Instant::now() + Duration::from_millis(ms);
        while Instant::now() < end {
            self.drive();
        }
    }
    fn quiet(&mut self) {
        // Client input and server painting are asynchronous. Require both a
        // quiet display and enough guest work to drain a pending redraw before
        // asserting that a paused/idle frame stays unchanged.
        let end = Instant::now() + Duration::from_secs(60);
        let mut revision = (self.vm.presented_frames, self.log.len());
        let mut since = Instant::now();
        let mut instructions = self.vm.executed_instructions;
        loop {
            self.drive();
            let current = (self.vm.presented_frames, self.log.len());
            if current != revision {
                revision = current;
                since = Instant::now();
                instructions = self.vm.executed_instructions;
            }
            if since.elapsed() >= Duration::from_millis(500)
                && self.vm.executed_instructions - instructions >= 4_000_000
            {
                return;
            }
            assert!(
                Instant::now() < end,
                "display did not become idle\n{}",
                self.log
            );
        }
    }
    fn key(&mut self, key: u16) -> usize {
        let at = self.log.len();
        self.vm.devices.accept_events([
            InputTransition::Key { key, down: true },
            InputTransition::Key { key, down: false },
        ]);
        at
    }
    fn pixels(&self) -> Vec<u16> {
        self.vm
            .rgb565_display
            .as_ref()
            .unwrap()
            .get_state()
            .lock()
            .unwrap()
            .front_buffer
            .as_ref()
            .clone()
    }
    fn snapshot(&self, name: &str) {
        let mut ppm = b"P6\n640 480\n255\n".to_vec();
        for p in self.pixels() {
            let r = (p >> 11) & 31;
            let g = (p >> 5) & 63;
            let b = p & 31;
            ppm.extend_from_slice(&[
                ((r << 3) | (r >> 2)) as u8,
                ((g << 2) | (g >> 4)) as u8,
                ((b << 3) | (b >> 2)) as u8,
            ]);
        }
        std::fs::write(self.output.join(name), ppm).unwrap();
    }
    fn wait_pixels(&mut self, test: impl Fn(&[u16]) -> bool) {
        let end = Instant::now() + Duration::from_secs(60);
        while !test(&self.pixels()) {
            if Instant::now() >= end {
                self.snapshot("timeout.ppm");
            }
            assert!(
                Instant::now() < end,
                "waiting for pixels; state={:?} instructions={} input_events={}\n{}",
                self.vm.state,
                self.vm.executed_instructions,
                self.vm.devices.input.events.len(),
                self.log
            );
            self.drive();
        }
    }
}
impl Guest {
    fn geometry(&self) -> (i32, i32, i32) {
        let line = self
            .log
            .lines()
            .rev()
            .find(|s| s.starts_with("SHIKAKU_PAINT "))
            .unwrap();
        let v: Vec<i32> = line
            .split_whitespace()
            .skip(1)
            .map(|s| s.parse().unwrap())
            .collect();
        (v[1], v[2] + 12, v[3] + 8)
    }
    fn drag(&mut self, r: &[i32], button: u16) {
        let (tile, bx, by) = self.geometry();
        let x = bx + r[0] * tile + tile / 2;
        let y = by + r[1] * tile + tile / 2;
        let xx = bx + (r[0] + r[2] - 1) * tile + tile / 2;
        let yy = by + (r[1] + r[3] - 1) * tile + tile / 2;
        self.vm.devices.accept_events([
            InputTransition::Motion { x, y },
            InputTransition::Button {
                button,
                down: true,
                x,
                y,
            },
        ]);
        self.settle(100);
        let at = self.log.len();
        self.vm.devices.accept_events([
            InputTransition::Motion { x: xx, y: yy },
            InputTransition::Button {
                button,
                down: false,
                x: xx,
                y: yy,
            },
        ]);
        self.until("SHIKAKU_CHANGE ", at);
        self.until("SHIKAKU_PAINT ", at);
        self.quiet();
    }
}
#[test]
#[ignore = "run ports/shikaku/tests/guests.py --target ripple"]
fn shikaku_input_zoom_resize_and_persistence() {
    let build = PathBuf::from(std::env::var_os("SHIKAKU_TEST_BUILD").unwrap());
    let output = PathBuf::from(std::env::var_os("SHIKAKU_TEST_OUTPUT").unwrap());
    let disk = PathBuf::from(std::env::var_os("SHIKAKU_TEST_DISK").unwrap());
    let boot = || {
        let mut vm = VM::with_options(64000, 16384000, Some(disk.clone()));
        vm.jit_enabled = true;
        vm.host_input_enabled = false;
        vm.devices
            .network
            .configure_host(Some(std::ffi::OsStr::new("off")))
            .unwrap();
        vm.load_binary(&std::fs::read(build.join("bios.bin")).unwrap())
            .unwrap();
        vm.rgb565_display = Some(RGB565Display::new());
        Guest {
            vm,
            log: String::new(),
            output: output.clone(),
        }
    };
    let mut g = boot();
    g.until("riplix# ", 0);
    g.command("SHIKAKU_TRACE=1; export SHIKAKU_TRACE\nNANOX_KEEP_TTY=1; export NANOX_KEEP_TTY\nnano-X -p &\nserver=$!\n","Nano-X ready");
    g.command("nxcheck\n", "riplix# ");
    assert!(g.log.contains("NANOX_CLIENT_OK"));
    g.command("shikaku 5 &\ngame=$!\n", "SHIKAKU_READY");
    g.wait_pixels(|p| p.iter().filter(|&&v| v == 0xee0d).count() > 15);
    g.quiet();
    g.snapshot("initial.ppm");
    let puzzles: serde_json::Value =
        serde_json::from_str(include_str!("../../ports/shikaku/puzzles.json")).unwrap();
    let solution: Vec<Vec<i32>> = puzzles[0]["solution"]
        .as_array()
        .unwrap()
        .iter()
        .map(|r| {
            r.as_array()
                .unwrap()
                .iter()
                .map(|v| v.as_i64().unwrap() as i32)
                .collect()
        })
        .collect();
    g.drag(&solution[0], 0);
    assert!(g.log.contains("SHIKAKU_CHANGE 0 1 1 0"));
    g.snapshot("rectangle.ppm");
    g.drag(&solution[0], 1);
    assert!(g.log.contains("SHIKAKU_CHANGE 0 0 2 0"));
    let at = g.key(24);
    g.until("SHIKAKU_CHANGE 0 1 1 0", at);
    g.quiet();
    let at = g.key(28);
    g.until("SHIKAKU_CHANGE 0 0 2 0", at);
    g.quiet();
    for r in &solution {
        g.drag(r, 0);
    }
    assert!(g
        .log
        .contains(&format!("SHIKAKU_SAVE 0 {} 1", solution.len())));
    g.snapshot("solved.ppm");
    let at = g.log.len();
    g.command("klotskictl 480 340\n", "KLOTSKI_CONTROL_OK");
    g.until("SHIKAKU_RESIZE 480 340", at);
    g.quiet();
    g.snapshot("resized.ppm");
    let at = g.key(46);
    g.until("SHIKAKU_PAINT ", at);
    g.quiet();
    g.snapshot("zoom.ppm");
    let at = g.key(9);
    g.until("SHIKAKU_PAINT ", at);
    g.quiet();
    let at = g.key(20);
    g.until("SHIKAKU_EXIT 0", at);
    g.command(
        "wait $game\nshikaku 25 --seed 1 &\ngame=$!\n",
        "SHIKAKU_SEED 25 1 4081565494",
    );
    g.quiet();
    g.snapshot("generated.ppm");
    let at = g.key(20);
    g.until("SHIKAKU_EXIT 0", at);
    g.command(
        "wait $game\nkill $server\nwait $server\nshutdown\n",
        "power off.",
    );
    while !matches!(g.vm.state, VMState::Halted) {
        g.drive();
    }
    std::fs::write(output.join("first-boot.log"), &g.log).unwrap();
    drop(g);
    let mut g = boot();
    g.until("riplix# ", 0);
    g.command("SHIKAKU_TRACE=1; export SHIKAKU_TRACE\nNANOX_KEEP_TTY=1; export NANOX_KEEP_TTY\nnano-X -p &\nserver=$!\n","Nano-X ready");
    g.command("shikaku 5 &\ngame=$!\n", "SHIKAKU_READY");
    assert!(g
        .log
        .contains(&format!("SHIKAKU_PUZZLE 0 5 {}", solution.len())));
    g.quiet();
    g.snapshot("solved-after-reboot.ppm");
    let at = g.key(20);
    g.until("SHIKAKU_EXIT 0", at);
    g.command(
        "wait $game\nshikaku 25 &\ngame=$!\n",
        "SHIKAKU_SEED 25 1 4081565494",
    );
    g.quiet();
    g.snapshot("25-fit.ppm");
    let before = g.geometry().0;
    let at = g.key(46);
    g.until("SHIKAKU_PAINT ", at);
    g.quiet();
    assert!(g.geometry().0 > before);
    g.snapshot("25-zoom.ppm");
    let at = g.key(58);
    g.settle(200);
    g.snapshot("help.ppm");
    g.key(41);
    g.until("SHIKAKU_PAINT ", at);
    g.quiet();
    let idle = g.pixels();
    let frames = g.vm.presented_frames;
    g.settle(350);
    assert_eq!(idle, g.pixels());
    assert_eq!(frames, g.vm.presented_frames);
    let at = g.key(20);
    g.until("SHIKAKU_EXIT 0", at);
    g.command(
        "wait $game\nkill $server\nwait $server\nshutdown\n",
        "power off.",
    );
    while !matches!(g.vm.state, VMState::Halted) {
        g.drive();
    }
    println!("Shikaku/RVM JIT: real mouse drawing/erasing, undo/redo, win, resize, zoom/fit, largest board, idle, reboot persistence and cleanup passed");
}
