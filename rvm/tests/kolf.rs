//! Real Kolf acceptance on JIT RVM. Run ports/kolf/tests/guests.py.
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
    fn stroke(&mut self) -> usize {
        let at = self.log.len();
        self.vm.devices.accept_events([InputTransition::Key {
            key: 44,
            down: true,
        }]);
        // Hold through the automatic maximum so host scheduling and the
        // emulated renderer cannot change the fixture's launch speed.
        self.until("KOLF_SWING 0 3", at);
        self.vm.devices.accept_events([InputTransition::Key {
            key: 44,
            down: false,
        }]);
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
    fn client_unchanged(&self, before: &[u16]) -> bool {
        let after = self.pixels();
        (4..434).all(|y| after[y * 640 + 8..y * 640 + 618] == before[y * 640 + 8..y * 640 + 618])
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
#[ignore = "run ports/kolf/tests/guests.py --target ripple"]
fn kolf_physics_pixels_round_and_cleanup() {
    let build = PathBuf::from(std::env::var_os("KOLF_TEST_BUILD").unwrap());
    let output = PathBuf::from(std::env::var_os("KOLF_TEST_OUTPUT").unwrap());
    let mut vm = VM::with_options(
        64000,
        16384000,
        Some(PathBuf::from(std::env::var_os("KOLF_TEST_DISK").unwrap())),
    );
    vm.jit_enabled = true;
    vm.host_input_enabled = false;
    vm.devices
        .network
        .configure_host(Some(std::ffi::OsStr::new("off")))
        .unwrap();
    vm.load_binary(&std::fs::read(build.join("bios.bin")).unwrap())
        .unwrap();
    vm.rgb565_display = Some(RGB565Display::new());
    let mut g = Guest {
        vm,
        log: String::new(),
        output,
    };
    g.until("riplix# ", 0);
    g.command("kolfcheck /courses/Practice.kolf\n", "failures=0");
    g.command("kolf --check-course\n", "KOLF_COURSE_OK holes=9");
    g.command("kolf --check-course Hard\n", "KOLF_COURSE_OK holes=18");
    g.command("kolf --benchmark 100\n", "KOLF_BENCH steps=900");
    g.command("KOLF_TRACE=1; export KOLF_TRACE\nNANOX_KEEP_TTY=1; export NANOX_KEEP_TTY\nnano-X -p &\nserver=$!\n","Nano-X ready");
    // Move before the first client paint: a motion during that paint can leave
    // the old software cursor in the captured baseline until the next repaint.
    g.vm.devices
        .accept_events([InputTransition::Motion { x: 635, y: 475 }]);
    g.settle(500);
    let start = Instant::now();
    g.command("kolf &\ngame=$!\n", "KOLF_READY");
    g.wait_pixels(|p| p[300 * 640 + 200] != 0);
    println!("Practice startup to visible frame: {:?}", start.elapsed());
    g.settle(300);
    g.snapshot("practice.ppm");
    let before = g.pixels();
    g.key(58);
    g.wait_pixels(|p| p[150 * 640 + 100] != before[150 * 640 + 100]);
    g.snapshot("help.ppm");
    g.key(41);
    // Compare the application rectangle; serial diagnostics may update the
    // uncovered console outside it while Nano-X keeps the tty enabled.
    g.wait_pixels(|p| {
        (4..434).all(|y| p[y * 640 + 8..y * 640 + 618] == before[y * 640 + 8..y * 640 + 618])
    });
    g.stroke();
    g.settle(200);
    let at = g.key(19);
    g.until("KOLF_PAUSE 1", at);
    g.until("KOLF_PAINTED 1 1 ", at);
    g.quiet();
    let paused = g.pixels();
    g.snapshot("paused.ppm");
    g.settle(350);
    g.snapshot("paused-again.ppm");
    assert!(g.client_unchanged(&paused), "Paused ball must not move");
    g.key(21); // retry clears pause and any pending stroke
    g.key(4); // optional advanced putting
    let at = g.key(44);
    g.until("KOLF_SWING 1 1", at);
    // Top of the dial's white outer ring beside Practice's ball at (68,257).
    g.wait_pixels(|p| p[179 * 640 + 152] == 0xe73b);
    g.snapshot("circular-power.ppm");
    let at = g.key(44);
    g.until("KOLF_SWING 1 2", at);
    g.key(44);
    // A slow display can let the accuracy window expire before the third
    // press is delivered; that must also finish the already-started stroke.
    g.until("KOLF_SHOT", at);
    g.key(21);
    g.key(4); // restore classic putting
    let at = g.key(20);
    g.until("KOLF_EXIT", at);
    g.command(
        "wait $game\nkolf /courses/advanced.kolf &\ngame=$!\n",
        "KOLF_READY",
    );
    g.settle(300);
    let initial = g.pixels();
    g.wait_pixels(|p| p != initial.as_slice());
    g.snapshot("advanced.ppm");
    let at = g.key(19);
    g.until("KOLF_PAUSE 1", at);
    g.until("KOLF_PAINTED 1 1 ", at);
    g.quiet();
    let paused = g.pixels();
    g.snapshot("advanced-paused.ppm");
    g.settle(400);
    g.snapshot("advanced-paused-again.ppm");
    assert!(
        g.client_unchanged(&paused),
        "Paused moving obstacles must freeze"
    );
    let at = g.key(20);
    g.until("KOLF_EXIT", at);
    g.command(
        "wait $game\nkolf /courses/edge.kolf &\ngame=$!\n",
        "KOLF_READY",
    );
    g.vm.devices
        .accept_events([InputTransition::Motion { x: 158, y: 228 }]);
    let at = g.stroke();
    g.until("aim=256,0", at);
    g.snapshot("edge-aim.ppm");
    let at = g.key(20);
    g.until("KOLF_EXIT", at);
    g.command(
        "wait $game\nkolf /courses/water.kolf &\ngame=$!\n",
        "KOLF_READY",
    );
    for choice in 0..3 {
        g.vm.devices
            .accept_events([InputTransition::Motion { x: 238, y: 228 }]);
        let at = g.stroke();
        g.until("KOLF_STATE 1 5 2 ", at);
        g.until("KOLF_PAINTED 1 0 5", at);
        g.quiet();
        g.snapshot("water-choice.ppm");
        let frozen = g.pixels();
        g.settle(400);
        assert!(
            g.client_unchanged(&frozen),
            "Water decision must freeze the scene"
        );
        if choice == 2 {
            let at = g.key(24);
            g.until("KOLF_UNDO 1 0 80000 200000", at);
            continue;
        }
        if choice == 1 {
            g.key(81);
        }
        let at = g.key(40);
        g.until(&format!("KOLF_RECOVER {choice} 2 "), at);
        if choice == 1 {
            g.until("KOLF_RECOVER 1 2 80000 200000", at);
        }
        let at = g.log.len();
        if choice == 0 {
            g.key(24);
        } else {
            g.vm.devices.accept_events([
                InputTransition::Key {
                    key: 224,
                    down: true,
                },
                InputTransition::Key {
                    key: 29,
                    down: true,
                },
                InputTransition::Key {
                    key: 29,
                    down: false,
                },
                InputTransition::Key {
                    key: 224,
                    down: false,
                },
            ]);
        }
        g.until("KOLF_UNDO 1 0 80000 200000", at);
    }
    let at = g.key(20);
    g.until("KOLF_EXIT", at);
    g.command(
        "wait $game\nkolf /courses/straight.kolf &\ngame=$!\n",
        "KOLF_READY",
    );
    for i in 1..=9 {
        let at = g.stroke();
        g.until(&format!("KOLF_STATE {i} 2 1 "), at);
        if i == 1 {
            let at = g.key(24);
            g.until("KOLF_UNDO 1 0 100000 200000", at);
            let at = g.stroke();
            g.until("KOLF_STATE 1 2 1 ", at);
        }
        if i < 9 {
            let at = g.key(40);
            g.until(&format!("KOLF_HOLE {} ", i + 1), at);
        }
    }
    g.until("KOLF_PAINTED 9 0 ", 0);
    g.quiet();
    g.snapshot("round-complete.ppm");
    let at = g.key(17);
    g.until("KOLF_HOLE 1 ", at);
    g.until("KOLF_PAINTED 1 0 ", at);
    g.quiet();
    let before = g.pixels();
    let frames = g.vm.presented_frames;
    g.settle(400);
    assert!(g.client_unchanged(&before), "Idle board must be stable");
    assert_eq!(g.vm.presented_frames, frames, "Idle game must not repaint");
    let at = g.key(20);
    g.until("KOLF_EXIT", at);
    g.command(
        "wait $game\necho KOLF_STATUS:$?\nkill $server\nwait $server\nshutdown\n",
        "power off.",
    );
    assert!(g.log.contains("KOLF_STATUS:0"), "{}", g.log);
    while !matches!(g.vm.state, VMState::Halted) {
        g.drive();
    }
    println!("Kolf/RVM: physics, courses, putting, cursor aiming at border, undo, water recovery, modal freeze, pause, 9-hole round, idle and cleanup passed");
}
