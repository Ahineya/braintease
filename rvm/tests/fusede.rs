//! Opt-in real-kernel FuseDE protocol, pixels, controls and idle checks.
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
        let bytes = self.vm.get_output();
        if !bytes.is_empty() {
            self.log.push_str(&String::from_utf8_lossy(&bytes));
            std::fs::write(self.output.join("rvm.log"), &self.log).unwrap();
        }
        assert!(
            !matches!(self.vm.state, VMState::Error(_)),
            "{:?}\n{}",
            self.vm.state,
            self.log
        );
        assert!(
            !self.log.contains("panic:")
                && !self.log.contains("failed line")
                && !self.log.contains("NXCHECK_")
                && !self.log.contains("blit snapshot allocation failed")
                && !self.log.contains("NXR_BUFFER_FAILED")
                && !self.log.contains("GsError"),
            "{}",
            self.log
        );
    }
    fn until(&mut self, marker: &str) {
        let start = Instant::now();
        while !self.log.contains(marker) {
            assert!(
                start.elapsed() < Duration::from_secs(60),
                "waiting for {marker}\n{}",
                self.log
            );
            assert!(
                !matches!(self.vm.state, VMState::Halted),
                "halted waiting for {marker}\n{}",
                self.log
            );
            self.drive();
        }
    }
    fn settle(&mut self, ms: u64) {
        let start = Instant::now();
        while start.elapsed() < Duration::from_millis(ms) {
            self.drive();
        }
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
        let bytes: Vec<u8> = self.pixels().iter().flat_map(|p| p.to_le_bytes()).collect();
        std::fs::write(self.output.join(name), bytes).unwrap();
    }
    fn command(&mut self, command: &str, marker: &str) {
        self.vm.push_input_string(command);
        self.until(marker);
    }
    fn key(&mut self, key: u16) {
        self.vm.devices.accept_events([
            InputTransition::Key { key, down: true },
            InputTransition::Key { key, down: false },
        ]);
        self.settle(300);
    }
    fn changed(&mut self, before: &[u16], label: &str) {
        let start = Instant::now();
        let mut frame = u64::MAX;
        loop {
            if frame != self.vm.presented_frames {
                frame = self.vm.presented_frames;
                if self.pixels() != before {
                    return;
                }
            }
            assert!(
                start.elapsed() < Duration::from_secs(10),
                "{label}\n{}",
                self.log
            );
            self.drive();
        }
    }
    fn drag(&mut self, x: i32, y: i32, dx: i32, dy: i32) {
        self.vm.devices.accept_events([
            InputTransition::Motion { x, y },
            InputTransition::Button {
                button: 0,
                down: true,
                x,
                y,
            },
        ]);
        self.settle(150);
        self.vm.devices.accept_events([InputTransition::Motion {
            x: x + dx,
            y: y + dy,
        }]);
        self.settle(250);
        self.vm.devices.accept_events([InputTransition::Button {
            button: 0,
            down: false,
            x: x + dx,
            y: y + dy,
        }]);
        self.settle(250);
    }
}

#[test]
#[ignore = "run through ports/fusede/tools/test_showcase.py with a disposable disk"]
fn colour_resources_and_showcase() {
    let build = PathBuf::from(std::env::var_os("FDE_RVM_BUILD").unwrap());
    let disk = PathBuf::from(std::env::var_os("FDE_TEST_DISK").unwrap());
    let output = PathBuf::from(std::env::var_os("FDE_TEST_OUTPUT").unwrap());
    let mut vm = VM::with_options(64000, 16384000, Some(disk));
    vm.host_input_enabled = false;
    vm.jit_enabled = true;
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
    g.until("fuzix# ");
    g.vm.push_input_string("FDE_TRACE=1\nexport FDE_TRACE\n");
    g.command("nano-X -p &\ns=$!\n", "Nano-X ready");
    g.command("fdeshowcase --check &\np=$!\n", "FDE_IMAGE_CHECKS_OK");
    g.settle(300);
    g.snapshot("rvm-check.rgb565");
    g.command(
        "kill -10 $p\nwait $p\necho CHECK_STATUS:$?\n",
        "CHECK_STATUS:0",
    );
    // Existing clients/quotas still work after image errors and disconnects.
    g.command("nxcheck\necho NXC_STATUS:$?\n", "NXC_STATUS:0");
    g.command(
        "nanowm &\nm=$!\nfdectl --check\necho WS_STATUS:$?\n",
        "WS_STATUS:0",
    );
    assert!(g.log.contains("FDE_WORKSPACE_CHECKS_OK"));
    g.command("fdeshowcase &\na=$!\n", "FDE_SHOWCASE_READY");
    g.settle(500);
    // Click unused client content to establish focus after asynchronous WM mapping.
    g.vm.devices.accept_events([
        InputTransition::Motion { x: 520, y: 360 },
        InputTransition::Button {
            button: 0,
            down: true,
            x: 520,
            y: 360,
        },
        InputTransition::Button {
            button: 0,
            down: false,
            x: 520,
            y: 360,
        },
    ]);
    g.settle(300);
    let before = g.pixels();
    g.key(81);
    g.until("FDE_INPUT 1 0");
    g.changed(&before, "Down did not update selection");
    let selected = g.pixels();
    g.key(43);
    g.until("FDE_INPUT 1 1");
    g.changed(&selected, "Tab did not change focus");
    g.settle(500);
    let frames = g.vm.presented_frames;
    g.settle(500);
    assert_eq!(
        frames, g.vm.presented_frames,
        "idle showcase keeps repainting"
    );
    // Mapping/reparenting can precede the first visible repaint. Capture only
    // after confirmed input draws and the idle check have settled the screen.
    g.snapshot("rvm-showcase.rgb565");
    g.command("nxcheck --frame\necho FRAME_STATUS:$?\n", "FRAME_STATUS:0");
    let before_move = g.pixels();
    g.drag(100, 15, 30, 20);
    g.changed(&before_move, "showcase title drag did not move pixels");
    g.drag(130, 35, -30, -20);
    // Processes is nxterm running top. Keep the skinned window alive while
    // opening it, then ensure its decoration is direct and the server recovers
    // from exhaustion of optional backing stores without returning window 0.
    let process_start = g.log.len();
    g.command(
        "NANOX_TRACE=1\nexport NANOX_TRACE\nnxterm /bin/top &\nt=$!\n",
        "NXTERM_READY",
    );
    g.settle(500);
    assert!(
        !g.log[process_start..].contains("NXTERM_EXIT"),
        "Processes exited before interaction\n{}",
        g.log
    );
    g.command(
        "nxcheck --frame\necho PROCESS_FRAME_STATUS:$?\n",
        "PROCESS_FRAME_STATUS:0",
    );
    g.command(
        "nxcheck --pressure\necho PRESSURE_STATUS:$?\n",
        "PRESSURE_STATUS:0",
    );
    g.command(
        "kill $t\nwait $t\necho PROCESS_STATUS:$?\n",
        "PROCESS_STATUS:0",
    );
    g.settle(300);
    // Closing the terminal restores focus by clicking the showcase content.
    g.vm.devices.accept_events([
        InputTransition::Motion { x: 520, y: 360 },
        InputTransition::Button {
            button: 0,
            down: true,
            x: 520,
            y: 360,
        },
        InputTransition::Button {
            button: 0,
            down: false,
            x: 520,
            y: 360,
        },
    ]);
    g.settle(300);
    g.key(41);
    g.until("FDE_SHOWCASE_CLOSED");
    // A dead manager must not recursively destroy the applications reparented
    // into its frames. The next manager adopts the same logical client ID.
    g.command("fdectl --hold &\nh=$!\n", "FDE_WORKSPACE_HOLD");
    let hold_id: u16 = g
        .log
        .rsplit("FDE_WORKSPACE_HOLD ")
        .next()
        .unwrap()
        .split_whitespace()
        .next()
        .unwrap()
        .parse()
        .unwrap();
    g.settle(400);
    // Real frame buttons and the global keyboard switcher use the same state
    // as the public service. A minimized window must always be recoverable.
    g.drag(220, 48, 0, 0);
    g.command(
        &format!("fdectl info {hold_id}\necho POINTER_MIN:$?\n"),
        "POINTER_MIN:0",
    );
    assert!(
        g.log.rsplit("Window ").next().unwrap().contains("state=20"),
        "{}",
        g.log
    );
    g.vm.devices.accept_events([InputTransition::Key {
        key: 226,
        down: true,
    }]);
    g.vm.devices.accept_events([InputTransition::Key {
        key: 43,
        down: true,
    }]);
    g.settle(700); // Holding the shortcut must not repeat Tab into the client.
    g.vm.devices.accept_events([
        InputTransition::Key {
            key: 43,
            down: false,
        },
        InputTransition::Key {
            key: 226,
            down: false,
        },
    ]);
    g.settle(200);
    assert!(!g.log.contains("FDE_WORKSPACE_KEY 9"), "{}", g.log);
    g.command(
        &format!("fdectl info {hold_id}\necho KEY_RESTORE:$?\n"),
        "KEY_RESTORE:0",
    );
    assert!(
        g.log.rsplit("Window ").next().unwrap().contains("state=19"),
        "{}",
        g.log
    );
    g.drag(244, 48, 0, 0);
    g.command(
        &format!("fdectl info {hold_id}\necho POINTER_MAX:$?\n"),
        "POINTER_MAX:0",
    );
    assert!(
        g.log.rsplit("Window ").next().unwrap().contains("state=27"),
        "{}",
        g.log
    );
    g.drag(599, 14, 0, 0);
    g.drag(282, 182, 40, 20);
    g.command(
        &format!("fdectl info {hold_id}\necho POINTER_SIZE:$?\n"),
        "POINTER_SIZE:0",
    );
    assert!(
        g.log.rsplit("Window ").next().unwrap().contains("280x140"),
        "{}",
        g.log
    );
    g.command(
        "kill -9 $m\nwait $m\nfdectl list\necho WM_GONE:$?\n",
        "WM_GONE:0\r\n",
    );
    let after_death = g.log.rsplit("wait $m\r\n").next().unwrap();
    assert!(
        after_death.contains(&format!("{hold_id:<6}")) && after_death.contains("Workspace probe"),
        "{}",
        g.log
    );
    g.command(
        "nanowm &\nm=$!\nfdectl --check\necho WM_RESTARTED:$?\n",
        "WM_RESTARTED:0",
    );
    g.command(
        &format!("fdectl activate {hold_id}\nfdectl list\necho WM_RECOVERED:$?\n"),
        "WM_RECOVERED:0\r\n",
    );
    assert!(
        g.log
            .rsplit("fdectl list\r\n")
            .next()
            .unwrap()
            .contains("Workspace probe"),
        "{}",
        g.log
    );
    g.snapshot("rvm-workspace.rgb565");
    g.vm.push_input_string("kill $h\nwait $h\n");
    g.command(
        "wait $a\necho APP_STATUS:$?\nkill $m\nwait $m\nkill $s\nwait $s\nshutdown\n",
        "disk synced; power off.",
    );
    while !matches!(g.vm.state, VMState::Halted) {
        g.drive();
    }
    assert!(g.log.contains("APP_STATUS:0"), "{}", g.log);
    println!("FuseDE RVM: colour protocol, ownership/quota/recovery, controls, idle and session checks passed");
}
