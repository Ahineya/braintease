//! Real Nano-X NetHack acceptance. Run ports/nethack/test_graphics.py ripple.
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
    more_seen: usize,
}
impl Guest {
    fn drive(&mut self) {
        self.vm.run_batch(65536).expect("guest execution");
        let output = self.vm.get_output();
        if !output.is_empty() {
            self.log.push_str(&String::from_utf8_lossy(&output));
            std::fs::write(self.output.join("guest.log"), &self.log).unwrap();
            for bad in [
                "panic:",
                "GsError",
                "NXR_BUFFER_FAILED",
                "Program initialization has failed",
            ] {
                assert!(!self.log.contains(bad), "{}", self.log);
            }
        }
        assert!(
            self.vm.device_error.is_none(),
            "{:?}\n{}",
            self.vm.device_error,
            self.log
        );
        assert!(
            !matches!(self.vm.state, VMState::Error(_)),
            "{:?}\n{}",
            self.vm.state,
            self.log
        );
    }
    fn until(&mut self, marker: &str, at: usize) {
        let end =
            Instant::now() + Duration::from_secs(if marker == "NHNX_READY" { 600 } else { 180 });
        let mut progress = Instant::now();
        while !self.log[at..].contains(marker) {
            assert!(
                Instant::now() < end && !matches!(self.vm.state, VMState::Halted),
                "waiting for {marker}; state={:?} instructions={}\n{}",
                self.vm.state,
                self.vm.executed_instructions,
                self.log
            );
            self.drive();
            if self.log[self.more_seen..].contains("NHNX_MORE") {
                self.more_seen = self.log.len();
                self.key(44, false, false);
            }
            if progress.elapsed() > Duration::from_secs(20) {
                eprintln!(
                    "Waiting for {marker}: {} instructions; state={:?}",
                    self.vm.executed_instructions, self.vm.state
                );
                progress = Instant::now();
            }
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
    fn key(&mut self, key: u16, shift: bool, ctrl: bool) -> usize {
        let at = self.log.len();
        let mut events = Vec::new();
        if shift {
            events.push(InputTransition::Key {
                key: 225,
                down: true,
            });
        }
        if ctrl {
            events.push(InputTransition::Key {
                key: 224,
                down: true,
            });
        }
        events.extend([
            InputTransition::Key { key, down: true },
            InputTransition::Key { key, down: false },
        ]);
        if ctrl {
            events.push(InputTransition::Key {
                key: 224,
                down: false,
            });
        }
        if shift {
            events.push(InputTransition::Key {
                key: 225,
                down: false,
            });
        }
        self.vm.devices.accept_events(events);
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
    fn drawing_ready(name: &str, p: &[u16]) -> bool {
        if matches!(name, "map.ppm" | "restored.ppm") {
            // The status is drawn after the map; the startup copyright
            // window has no status line.
            (350..400).any(|y| p[y * 640 + 20..y * 640 + 600].contains(&0xdf1b))
        } else if matches!(
            name,
            "inventory.ppm" | "help.ppm" | "history.ppm" | "drop.ppm" | "drop-selected.ppm"
        ) {
            // An overlay replaces the black map background throughout
            // this interior rectangle with the panel background.
            (90..320).all(|y| !p[y * 640 + 24..y * 640 + 600].contains(&0))
        } else {
            true
        }
    }
    fn snapshot(&mut self, name: &str) -> Vec<u16> {
        // Poll at a bounded rate so screenshot inspection does not dominate
        // execution when run_batch returns early for individual MMIO writes.
        // Exposures may schedule another paint, so require a stable final image.
        let end = Instant::now() + Duration::from_secs(120);
        let mut stable = Instant::now();
        let mut pixels = Vec::new();
        loop {
            self.settle(100);
            let next = self.pixels();
            if !Self::drawing_ready(name, &next) || next != pixels {
                stable = Instant::now();
            }
            pixels = next;
            if stable.elapsed() >= Duration::from_millis(1500) {
                break;
            }
            assert!(
                Instant::now() < end,
                "waiting for completed {name}\n{}",
                self.log
            );
        }
        let mut ppm = b"P6\n640 480\n255\n".to_vec();
        for p in &pixels {
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
        pixels
    }
    fn save(&mut self) {
        let at = self.key(22, true, false);
        self.until("NHNX_PROMPT Really save", at);
        self.key(28, false, false);
        self.until("NHNX_EXIT", at);
        self.command("wait $game\n", "riplix# ");
    }
}

#[test]
#[ignore = "run ports/nethack/test_graphics.py ripple"]
fn tiled_nethack_gameplay_and_restore() {
    let build = PathBuf::from(std::env::var_os("NETHACK_TEST_BUILD").unwrap());
    let output = PathBuf::from(std::env::var_os("NETHACK_TEST_OUTPUT").unwrap());
    let mut vm = VM::with_options(
        64000,
        16384000,
        Some(PathBuf::from(
            std::env::var_os("NETHACK_TEST_DISK").unwrap(),
        )),
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
        more_seen: 0,
    };
    g.until("riplix# ", 0);
    g.command("NETHACK_TRACE=1; export NETHACK_TRACE; NANOX_KEEP_TTY=1; export NANOX_KEEP_TTY\nnano-X -p &\nserver=$!\n","Nano-X ready");
    let start = Instant::now();
    let at = g.log.len();
    g.command(
        "nethack-nx -u Tiles -p Valkyrie -r human -@ &\ngame=$!\n",
        "NHNX_READY",
    );
    g.until("NHNX_TURN 1 ", at);
    let startup_seconds = start.elapsed().as_secs_f64();
    println!("NetHack startup: {startup_seconds:.2}s");
    g.vm.devices
        .accept_events([InputTransition::Motion { x: 639, y: 479 }]);
    let initial = g.snapshot("map.ppm");
    assert_eq!(g.snapshot("idle.ppm"), initial, "Idle redraw");
    let at = g.key(12, false, false);
    g.until("NHNX_MENU", at);
    assert_ne!(g.snapshot("inventory.ppm"), initial);
    let at = g.key(41, false, false);
    g.until("NHNX_TURN 1 ", at);
    let at = g.key(58, false, false);
    g.until("NHNX_MENU", at);
    g.snapshot("help.ppm");
    let at = g.key(41, false, false);
    g.until("NHNX_TURN 1 ", at);
    let at = g.key(7, false, true);
    g.until("NHNX_PROMPT In what direction", at);
    g.key(79, false, false);
    g.until("NHNX_TURN 2 ", at);
    g.snapshot("turn.ppm");
    let at = g.key(19, false, true);
    g.until("Message history", at);
    g.snapshot("history.ppm");
    let at = g.key(41, false, false);
    g.until("NHNX_TURN 2 ", at);
    let at = g.key(7, true, false);
    g.until("NHNX_MENU", at);
    g.snapshot("drop.ppm");
    g.key(44, false, false);
    g.snapshot("drop-selected.ppm");
    let at = g.key(41, false, false);
    g.until("NHNX_TURN 2 ", at);
    let at = g.key(32, true, false);
    g.until("NHNX_PROMPT #", at);
    g.snapshot("command.ppm");
    let at = g.key(41, false, false);
    g.until("NHNX_TURN 2 ", at);
    g.save();
    let at = g.log.len();
    g.command("nethack-nx -u Tiles &\ngame=$!\n", "NHNX_READY");
    g.until("NHNX_TURN 2 ", at);
    g.snapshot("restored.ppm");
    g.save();
    g.command("kill $server\nwait $server\nshutdown\n", "power off.");
    std::fs::write(
        g.output.join("report.json"),
        format!("{{\"status\":\"passed\",\"startup_seconds\":{startup_seconds:.2}}}\n"),
    )
    .unwrap();
}
