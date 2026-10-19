//! Real Klotski acceptance on JIT RVM. Run ports/klotski/tests/guests.py.
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
#[test]
#[ignore = "run ports/klotski/tests/guests.py --target ripple"]
fn klotski_input_pixels_history_and_persistence() {
    let build = PathBuf::from(std::env::var_os("KLOTSKI_TEST_BUILD").unwrap());
    let output = PathBuf::from(std::env::var_os("KLOTSKI_TEST_OUTPUT").unwrap());
    let disk = PathBuf::from(std::env::var_os("KLOTSKI_TEST_DISK").unwrap());
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
    g.command("KLOTSKI_TRACE=1; export KLOTSKI_TRACE\nNANOX_KEEP_TTY=1; export NANOX_KEEP_TTY\nnano-X -p &\nserver=$!\n", "Nano-X ready");
    g.vm.devices
        .accept_events([InputTransition::Motion { x: 635, y: 475 }]);
    g.settle(250);
    g.command(
        "klotski 1 &\ngame=$!\n",
        "KLOTSKI_READY 1 600 416 30 210 95",
    );
    g.wait_pixels(|p| p.iter().filter(|&&v| v == 0x3334).count() > 1000);
    g.quiet();
    g.snapshot("initial.ppm");
    let initial = g.pixels();
    // First board: move k from (1,5) to (3,5) with one coalesced drag.
    let x = 255;
    let y = 260;
    g.vm.devices.accept_events([
        InputTransition::Motion { x, y },
        InputTransition::Button {
            button: 0,
            down: true,
            x,
            y,
        },
    ]);
    g.settle(100);
    let at = g.log.len();
    g.vm.devices
        .accept_events([InputTransition::Motion { x: x + 60, y }]);
    g.until("KLOTSKI_MOVE 1 1 2 0", at);
    g.vm.devices.accept_events([InputTransition::Button {
        button: 0,
        down: false,
        x: x + 60,
        y,
    }]);
    g.until("KLOTSKI_SAVE 1 2 2", at);
    g.settle(200);
    g.snapshot("dragged.ppm");
    assert_ne!(initial, g.pixels());
    let at = g.key(80);
    g.until("KLOTSKI_MOVE 1 1 3 0", at);
    let at = g.key(80);
    g.until("KLOTSKI_MOVE 1 0 4 0", at);
    let at = g.key(24);
    g.until("KLOTSKI_MOVE 1 1 3 0", at);
    let at = g.key(28);
    g.until("KLOTSKI_MOVE 1 0 4 0", at);
    let at = g.key(24);
    g.until("KLOTSKI_MOVE 1 1 3 0", at);
    let at = g.key(20);
    g.until("KLOTSKI_EXIT 0", at);
    g.command("wait $game\nklotski &\ngame=$!\n", "KLOTSKI_READY");
    assert!(g.log.contains("KLOTSKI_LEVEL 1 1 3"));
    let at = g.key(28);
    g.until("KLOTSKI_MOVE 1 0 4 0", at);
    let at = g.log.len();
    g.command("klotskictl 480 340\n", "KLOTSKI_CONTROL_OK");
    g.until("KLOTSKI_RESIZE 480 340", at);
    g.until("KLOTSKI_PAINT 0 1 22", at);
    g.quiet();
    g.snapshot("resized.ppm");
    let at = g.log.len();
    g.command("klotskictl 600 416\n", "KLOTSKI_CONTROL_OK");
    g.until("KLOTSKI_RESIZE 600 416", at);
    g.until("KLOTSKI_PAINT 0 1 30", at);
    g.key(6);
    g.settle(100);
    g.snapshot("puzzles.ppm");
    g.key(79);
    g.key(79);
    for _ in 0..8 {
        g.key(81);
        g.settle(20);
    }
    let at = g.key(40);
    g.until("KLOTSKI_LEVEL 28 0 0", at);
    g.until("KLOTSKI_PAINT 0 28", at);
    g.quiet();
    g.snapshot("sunshine.ppm");
    let at = g.key(58);
    g.until("KLOTSKI_PAINT 2 28", at);
    g.quiet();
    g.snapshot("help.ppm");
    assert!(!g.log[at..].contains("KLOTSKI_EXIT"));
    let at = g.key(41);
    g.until("KLOTSKI_PAINT 0 28", at);
    g.quiet();
    let idle = g.pixels();
    let frames = g.vm.presented_frames;
    g.settle(350);
    assert!(idle == g.pixels(), "Idle board must not change");
    assert_eq!(frames, g.vm.presented_frames);
    let at = g.key(20);
    g.until("KLOTSKI_EXIT 0", at);
    let at = g.log.len();
    g.command(
        "wait $game\nKLOTSKI_HOME=/wins klotski 1 &\ngame=$!\n",
        "KLOTSKI_READY 1 600 416 30 210 95",
    );
    g.until("KLOTSKI_PAINT 0 1 30", at);
    g.quiet();
    // Final step of an independent 32-step BFS solution of the real puzzle.
    let (x, y) = (323, 328);
    g.vm.devices.accept_events([
        InputTransition::Motion { x, y },
        InputTransition::Button {
            button: 0,
            down: true,
            x,
            y,
        },
        InputTransition::Button {
            button: 0,
            down: false,
            x,
            y,
        },
    ]);
    g.settle(100);
    let at = g.key(79);
    g.until("KLOTSKI_MOVE 1 27 32 1", at);
    g.until("KLOTSKI_SAVE 1 32 32 27", at);
    g.until("KLOTSKI_PAINT 0 1", at);
    g.quiet();
    g.snapshot("solved.ppm");
    let at = g.key(20);
    g.until("KLOTSKI_EXIT 0", at);
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
    g.command("KLOTSKI_TRACE=1; export KLOTSKI_TRACE\nNANOX_KEEP_TTY=1; export NANOX_KEEP_TTY\nnano-X -p &\nserver=$!\n","Nano-X ready");
    g.command("KLOTSKI_HOME=/wins klotski 1 &\ngame=$!\n", "KLOTSKI_READY");
    assert!(g.log.contains("KLOTSKI_LEVEL 1 27 32"));
    g.wait_pixels(|p| p.iter().filter(|&&v| v == 0x3334).count() > 1000);
    g.quiet();
    g.snapshot("solved-after-reboot.ppm");
    let at = g.key(20);
    g.until("KLOTSKI_SAVE 1 32 32 27", at);
    g.until("KLOTSKI_EXIT 0", at);
    g.command(
        "wait $game\nkill $server\nwait $server\nshutdown\n",
        "power off.",
    );
    while !matches!(g.vm.state, VMState::Halted) {
        g.drive();
    }
    println!("Klotski/RVM JIT: drag, keys, resize, grouped moves, undo/redo, largest board, idle, win, scores across reboot and cleanup passed");
}
